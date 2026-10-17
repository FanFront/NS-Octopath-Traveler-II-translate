using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System.Collections.Generic;
using System.Linq;

namespace Ryujinx.Graphics.Shader.Translation.Optimizations
{
    // Resolve a handle only when every feasible incoming value agrees. In particular,
    // do not choose a convenient constant-buffer leaf from an arbitrary phi tree.
    class BindlessHandleResolver
    {
        private readonly Dictionary<INode, BasicBlock> _owners = new();

        public BindlessHandleResolver(BasicBlock[] blocks)
        {
            foreach (var block in blocks)
                foreach (var node in block.Operations)
                    _owners[node] = block;
        }

        private static bool Dominates(BasicBlock a, BasicBlock b)
        {
            for (var cursor = b; cursor != null; cursor = cursor.ImmediateDominator)
            {
                if (cursor == a) return true;
                if (cursor == cursor.ImmediateDominator) break;
            }
            return false;
        }

        private static bool Same(Operand a, Operand b)
        {
            return a == b || (a.Type == b.Type && a.Value == b.Value &&
                a.Type is OperandType.Constant or OperandType.ConstantBuffer);
        }

        private static bool SameCondition(Operand a, Operand b, int depth = 16)
        {
            if (Same(a, b)) return true;
            if (depth == 0 || a.AsgOp is not Operation x || b.AsgOp is not Operation y ||
                x.Inst != y.Inst || x.StorageKind != y.StorageKind || x.Index != y.Index ||
                x.SourcesCount != y.SourcesCount) return false;
            if (!IsPureCondition(x.Inst)) return false;
            for (int i = 0; i < x.SourcesCount; i++)
                if (!SameCondition(x.GetSource(i), y.GetSource(i), depth - 1)) return false;
            return true;
        }

        private static bool IsPureCondition(Instruction inst) => inst.ToString().StartsWith("Compare") ||
            inst is Instruction.BitwiseAnd or Instruction.BitwiseOr or Instruction.BitwiseExclusiveOr or
                Instruction.BitwiseNot or Instruction.LogicalNot or Instruction.UnpackHalf2x16;

        private static IEnumerable<(Operand Condition, bool Value)> EdgeFacts(BasicBlock from, BasicBlock to)
        {
            if (from.Next == from.Branch || from.GetLastOp() is not Operation branch ||
                branch.Inst is not Instruction.BranchIfTrue and not Instruction.BranchIfFalse) yield break;
            if (from.Next != to && from.Branch != to) yield break;
            yield return (branch.GetSource(0), (branch.Inst == Instruction.BranchIfTrue) == (from.Branch == to));
        }

        private static IEnumerable<(Operand Condition, bool Value)> BlockFacts(BasicBlock block)
        {
            for (var cursor = block; cursor != null; cursor = cursor.ImmediateDominator)
            {
                // A join successor is not evidence that the branch took that edge.
                if (cursor.Predecessors.Count == 1)
                    foreach (var fact in EdgeFacts(cursor.Predecessors[0], cursor)) yield return fact;
                if (cursor == cursor.ImmediateDominator) break;
            }
        }

        private bool AvailableBefore(Operand value, BasicBlock phiBlock)
        {
            if (value.Type is OperandType.Constant or OperandType.ConstantBuffer) return true;
            return value.AsgOp != null && _owners.TryGetValue(value.AsgOp, out var owner) &&
                owner != phiBlock && Dominates(owner, phiBlock);
        }

        private bool StableCondition(Operand condition, BasicBlock phiBlock, int depth = 16)
        {
            if (AvailableBefore(condition, phiBlock)) return true;
            if (depth == 0 || condition.AsgOp is not Operation op || !IsPureCondition(op.Inst)) return false;
            for (int i = 0; i < op.SourcesCount; i++)
                if (!StableCondition(op.GetSource(i), phiBlock, depth - 1)) return false;
            return true;
        }

        public bool IsUnreachable(BasicBlock block)
        {
            var facts = new List<(Operand Condition, bool Value)>(BlockFacts(block));
            for (int i = 0; i < facts.Count; i++)
                for (int j = i + 1; j < facts.Count; j++)
                    if (facts[i].Value != facts[j].Value && SameCondition(facts[i].Condition, facts[j].Condition) &&
                        StableCondition(facts[i].Condition, block) && StableCondition(facts[j].Condition, block)) return true;
            return false;
        }

        public Operand Resolve(Operand source, BasicBlock useBlock)
        {
            var facts = new List<(Operand Condition, bool Value)>(BlockFacts(useBlock));
            var visiting = new HashSet<Operand>();
            Operand Walk(Operand value)
            {
                if (value.AsgOp is not PhiNode phi) return value;
                // A cycle alone proves nothing; all concrete exits still have to agree.
                if (!visiting.Add(value)) return null;
                Operand result = null;
                foreach (int index in System.Linq.Enumerable.Range(0, phi.SourcesCount))
                {
                    bool excluded = false;
                    if (_owners.TryGetValue(phi, out var owner) && Dominates(owner, useBlock))
                    {
                        excluded = true;
                        foreach (var predecessor in phi.GetIncomingBlocks(index))
                        {
                            if (predecessor.Next != owner && predecessor.Branch != owner) continue;
                            bool conflict = false;
                            var incomingFacts = new List<(Operand Condition, bool Value)>(BlockFacts(predecessor));
                            incomingFacts.AddRange(EdgeFacts(predecessor, owner));
                            foreach (var incoming in incomingFacts)
                                foreach (var known in facts)
                                    if (incoming.Value != known.Value && SameCondition(incoming.Condition, known.Condition) &&
                                        StableCondition(known.Condition, owner)) conflict = true;
                            if (!conflict) { excluded = false; break; }
                        }
                    }
                    if (excluded) continue;
                    Operand candidate = Walk(phi.GetSource(index));
                    if (candidate == null) continue;
                    if (result == null) result = candidate;
                    else if (!Equivalent(result, candidate)) { result = value; break; }
                }
                visiting.Remove(value);
                return result ?? value;
            }
            return Walk(source) ?? source;
        }

        private static bool Equivalent(Operand a, Operand b)
        {
            if (Same(a, b)) return true;
            if (a.AsgOp is not Operation x || b.AsgOp is not Operation y ||
                x.Inst != Instruction.BitwiseOr || y.Inst != Instruction.BitwiseOr) return false;
            // Handles assembled from the same two immutable table entries are equal,
            // including the commuted form generated on other branches.
            return (Same(x.GetSource(0), y.GetSource(0)) && Same(x.GetSource(1), y.GetSource(1))) ||
                   (Same(x.GetSource(0), y.GetSource(1)) && Same(x.GetSource(1), y.GetSource(0)));
        }
    }
}
