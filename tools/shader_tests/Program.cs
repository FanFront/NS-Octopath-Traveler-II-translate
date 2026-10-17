using System;
using System.Linq;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using Ryujinx.Graphics.Shader.Translation.Optimizations;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

static class RegressionTests
{
    static int checks;
    static void Check(bool pass, string message) { checks++; if (!pass) throw new Exception(message); }
    static BasicBlock[] Blocks(int count) => Enumerable.Range(0, count).Select(i => new BasicBlock(i)).ToArray();
    static void Branch(BasicBlock from, BasicBlock next, BasicBlock taken, Operand condition)
    {
        from.Next=next; from.Branch=taken;
        from.Append(new Operation(Instruction.BranchIfTrue, null, condition));
    }
    static Operand Or(BasicBlock block, Operand a, Operand b)
    {
        var v=Local(); block.Append(new Operation(Instruction.BitwiseOr, v, a, b)); return v;
    }
    static Operand Compare(BasicBlock block, int offset)
    {
        var unpack=Local(); block.Append(new Operation(Instruction.UnpackHalf2x16, unpack, Cbuf(3,offset)));
        var result=Local(); block.Append(new Operation(Instruction.CompareEqual, result, unpack, Const(0))); return result;
    }
    static BindlessHandleResolver Resolver(BasicBlock[] blocks)
    {
        Dominance.FindDominators(new ControlFlowGraph(blocks)); return new(blocks);
    }
    static void Paths()
    {
        // Identical pure predicates computed twice; the handle's definition site
        // differs from the incoming edge. Query both outcomes and the joined path.
        for(int variant=0;variant<3;variant++)
        {
            var b=Blocks(7); var p=Compare(b[0],0); Branch(b[0],b[1],b[2],p);
            var t=Cbuf(2,0x5a);var s=Cbuf(2,0x15a);var handle=Or(b[2],t,s);
            b[1].Next=b[3];b[2].Next=b[3];
            var value=Local();var phi=new PhiNode(value);phi.AddSource(b[0],Const(17),b[1]);phi.AddSource(b[2],handle);b[3].Append(phi);
            var q=variant==0?p:Compare(b[3],variant==1?0:1);Branch(b[3],b[4],b[5],q);b[4].Next=b[6];b[5].Next=b[6];
            var resolver=Resolver(b);
            Check(resolver.Resolve(value,b[5])==(variant<2?handle:value),"taken edge and recomputed predicate");
            Check(resolver.Resolve(value,b[4]).Type==(variant<2?OperandType.Constant:OperandType.LocalVariable),"opposite branch must select other value");
            Check(resolver.Resolve(value,b[6])==value,"join is not evidence of either branch");
            Check(phi.GetBlock(0)==b[0]&&phi.GetIncomingBlocks(0).Single()==b[1],"definition and incoming edge remain distinct");
        }
    }
    static void Loops()
    {
        foreach(bool different in new[]{false,true})
        {
            var b=Blocks(4);b[0].Next=b[1];var condition=Local();var state=new PhiNode(condition);state.AddSource(b[0],Const(0));state.AddSource(b[2],Const(1));b[1].Append(state);Branch(b[1],b[2],b[3],condition);b[2].Next=b[1];
            var a=Or(b[0],Cbuf(2,0x5a),Cbuf(2,0x15a));
            var other=Or(b[2],Cbuf(2,0x15a),Cbuf(2,different?0x5c:0x5a));
            var v=Local();var phi=new PhiNode(v);phi.AddSource(b[0],a);phi.AddSource(b[2],other);b[1].Append(phi);
            var r=Resolver(b);Check(r.Resolve(v,b[3])==(different?v:a),"loop arms must agree, including commuted OR");
        }
        var c=Blocks(3);c[0].Next=c[1];Branch(c[1],c[2],c[1],Cbuf(3,0));
        var value=Local();var loop=new PhiNode(value);loop.AddSource(c[0],Cbuf(2,0x5a));loop.AddSource(c[1],value);c[1].Append(loop);
        Check(Resolver(c).Resolve(value,c[2]).Type==OperandType.ConstantBuffer,"self cycle with one concrete initializer");
        loop.SetSource(0,Undef());Check(Resolver(c).Resolve(value,c[2]).Type==OperandType.Undefined,"undefined initializer must not invent a handle");
    }
    static void IncomingEdges()
    {
        var b=Blocks(7);var cond=Cbuf(3,0);Branch(b[0],b[1],b[2],cond);
        Branch(b[2],b[3],b[4],Cbuf(3,1));b[1].Next=b[5];b[3].Next=b[5];b[4].Next=b[5];b[5].Next=b[6];
        var value=Local();var phi=new PhiNode(value);
        // The same definition feeds two different incoming edges; neither is lost.
        phi.AddSource(b[0],Cbuf(2,0x5a),b[1]);phi.AddSource(b[0],Cbuf(2,0x5a),b[3]);phi.AddSource(b[4],Cbuf(2,0x5c));b[5].Append(phi);
        Check(phi.SourcesCount==2&&phi.GetIncomingBlocks(0).Count()==2,"all incoming edges retained after definition deduplication");
        Check(Resolver(b).Resolve(value,b[6])==value,"different reachable handles cannot collapse");
    }
    static void Contradiction()
    {
        foreach(bool same in new[]{true,false})
        {
            var b=Blocks(5);Branch(b[0],b[1],b[4],Compare(b[0],0));
            Branch(b[1],b[2],b[3],Compare(b[1],same?0:1));b[2].Next=b[4];b[3].Next=b[4];
            var r=Resolver(b);Check(r.IsUnreachable(b[3])==same,"only contradictory immutable conditions prove unreachable");
            Check(!r.IsUnreachable(b[4]),"joined block remains reachable");
        }
    }
    static int Main()
    {
        Paths();Loops();IncomingEdges();Contradiction();
        Console.WriteLine($"PASS {checks} shader dataflow assertions");return 0;
    }
}
