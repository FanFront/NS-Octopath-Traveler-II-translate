# 上传清单（Git vs 迅雷云盘）

## 一、传 Git（源码级，可公开或私有仓库）

只传这些，**不要**传 APK / s2a / 密钥 / 游戏文件：

```
dist/README.md
dist/docs/BUILD-NOTES.md
dist/patches/0001-ot2-playable.patch
dist/scripts/env.sh
dist/scripts/build-game.sh
dist/ot2-playable.bundle          （可选，方便直接导入分支）
```

推荐做法（保留上游历史）：

```sh
git clone https://github.com/n06ody/switch2android my-fork
cd my-fork
git checkout 46bc3f9
git am /path/to/dist/patches/0001-ot2-playable.patch
git remote add mine https://<你的仓库>.git
git push -u mine HEAD:ot2-playable
```

或者用 bundle：

```sh
git clone https://github.com/n06ody/switch2android my-fork
cd my-fork
git fetch /path/to/dist/ot2-playable.bundle ot2-playable-20261018:ot2-playable
git checkout ot2-playable
git remote add mine https://<你的仓库>.git
git push -u mine ot2-playable
```

## 二、传迅雷云盘（私有，含游戏内容）

```
dist/apk/game.apk                              （4.7 MB，已可玩）
out-diag11/0100a3501946e000-v131072/game.s2a   （6.7 GB，必须与上面的 APK 同一次构建）
dist/icons/ot2-icon-256.png
dist/mipmaps/                                  （可选，图标源）
dist/README.md                                 （可选，方便以后回看）
```

> `game.apk` 与 `game.s2a` 必须成对；`package.json` 里的 `bundle_id` 标识它们来自同一构建。
> 云盘请设为**私有**，不要分享链接。

## 三、校验

```sh
shasum -a 256 dist/apk/game.apk
# 应等于 package.json 中的 apk_sha256:
# 0acb961634e4442aab9159707d1d3d650883e228da88c54fceef72462a017064
```

## 四、以后换机/换游戏

- 换机：装 `game.apk` → 首次启动选 `game.s2a` → 完成。
- 换游戏：装好工具链后 `dist/scripts/build-game.sh <title_id> <version> <input> <keys> <work> <out>`，
  图标按 README 5.3/5.4 提取替换。
