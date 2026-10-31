# Disk Expander

Windows 磁盘布局与空间管理工具（Qt 6 / C++17）。

最初的目标是“统合所有物理硬盘、把后面的空间分给前面的盘（主要是 C:）”。
在 Windows 客户端上，**跨物理盘扩展启动卷（C:）在架构上不可实现**：基本磁盘的卷只能位于单块磁盘的连续区域，启动卷也不能放进跨区卷/存储空间。因此这个项目最终落地为四件真实可用的能力：

1. **磁盘布局查看**：按容量比例绘制每块磁盘的分区，标注 EFI/MSR/恢复/未分配、剩余空间、系统盘。
2. **空间体检**：只读扫描 C:，给出可清理 / 可迁移 / 需谨慎的清单。
3. **释放空间**：关闭休眠、清理临时文件、把 Downloads / Tencent Files 迁移到其他盘（junction）。
4. **回滚与恢复**：一键把 junction 恢复成普通目录；迁移中断后下次启动可完成或撤销。

> 只读的磁盘布局部分不会修改磁盘。释放空间/迁移/回滚属于写操作，执行前请阅读下文的风险说明。

## 功能

### 磁盘布局

- 枚举卷、分区、未分配空间，按容量比例绘制
- 识别 EFI、MSR、恢复分区、动态磁盘、存储池成员
- 顶部显示系统盘（C:）剩余空间与使用率
- 点击任意分区查看详情和“可执行操作”结论（能否同盘扩展、是否需要离线等）
- 导出 JSON 报告（`--scan-json` 或工具栏按钮）

### 空间体检

- 系统项：休眠文件、页面文件、WinSxS、更新缓存、传递优化、回收站、Windows.old、转储等
- 用户项：Downloads、Desktop、Documents、Pictures、Videos、Music、AppData 大目录
- 程序项：Program Files / ProgramData 下的大目录
- 每项标注“可清理 / 可迁移 / 需谨慎”及建议

### 释放空间

- 关闭休眠（`powercfg /h off`，可逆）
- 清理临时文件、更新缓存、回收站
- 迁移目录到指定目标（默认 `D:\DiskExpander\Data`）：
  - `robocopy` 复制 → 字节数校验 → 原目录改名 `.migrating` → 建立 junction → 通过 junction 再校验 → 删除 C: 上的原目录
  - 任一步失败自动回滚；junction 校验失败时只删除 reparse point，不会误删 D: 数据

### 回滚与断点恢复

- **回滚迁移**：扫描 `%USERPROFILE%` 下指向迁移目标的 junction，删除链接并把数据搬回 C:
- **迁移日志**：迁移过程写入 `C:\ProgramData\DiskExpander\journal.json`（copying / copied / renamed / linked）
- 启动时如发现未完成日志，可选择“完成迁移”或“撤销并恢复”

## 重要限制

- **不能把其他物理磁盘的空间并入 C:**。这不是本工具的限制，而是 Windows 的卷/启动链限制；第三方分区工具同样做不到。
- 迁移依赖 **junction**。绝大多数程序透明，但个别备份/同步工具可能不识别；可用“回滚迁移”恢复。
- 迁移后原路径会显示为带箭头的文件夹，**不要手动删除这个箭头**，否则路径会断（数据仍在目标盘）。
- 修改分区表、BitLocker、动态磁盘、存储池等高风险场景**不在当前功能范围内**，工具只做只读展示。
- 页面文件、WinSxS、Package Cache 等系统项默认**不自动处理**，避免影响系统修复和更新回滚。

## 构建

环境：

- Windows 10/11 x64
- Qt 6.2+（开发使用 6.6.3 msvc2019_64）
- Visual Studio 2022（MSVC v143）
- CMake 3.16+、Ninja

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=D:/Qt/6.6.3/msvc2019_64
cmake --build build
```

程序带 `requireAdministrator` manifest，启动会弹 UAC。调试时请以管理员身份启动 Qt Creator，否则调试器无法附加。

GitHub Actions 会在 push / PR / tag 时自动构建，产物以 artifact 形式提供，配置见 `.github/workflows/build.yml`。

## 下载与签名

Release 里的 `Disk_Expander-0.3.0-win64.zip` **未做代码签名**。Windows 可能提示“未知发布者”，或出现 SmartScreen 拦截；首次运行请确认文件来自本仓库 Release，并可对照 `SHA256SUMS.txt` 校验。

生成校验文件：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\make-checksums.ps1
```

## 使用

1. 启动后先看磁盘布局；点击 C: 可看到“同盘/跨盘扩容”的真实结论。
2. 工具栏“空间体检”：只读扫描，生成清单。
3. 工具栏“释放空间”：按需勾选，目标目录默认 `D:\DiskExpander\Data`。
4. 工具栏“回滚迁移”：需要恢复时使用。

## 数据位置

| 内容 | 路径 |
|---|---|
| 迁移日志 | `C:\ProgramData\DiskExpander\journal.json` |
| 运行日志 | `%LOCALAPPDATA%\DiskExpander\logs\disk-expander.log` |
| 默认迁移目标 | `D:\DiskExpander\Data` |

## 调试参数

| 参数 | 作用 |
|---|---|
| `--scan-json <file>` | 扫描磁盘布局并写 JSON 后退出 |
| `--audit-json <file>` | 只读空间体检并写 JSON 后退出 |
| `--self-test` | 临时目录内跑“迁移 → 回滚”自测，弹窗显示结果 |
| `--screenshot <png>` | 离屏截图（配合 `--demo` 使用模拟数据） |
| `--demo` | 用模拟磁盘数据填充界面 |

## 风险说明

- 迁移/回滚会移动大量文件，执行前请关闭微信/QQ、下载工具等占用目录的程序。
- 迁移期间不要关机；中断后下次启动会提示恢复。
- 建议首次使用先在测试目录或虚拟机中验证。
- 本工具按“尽力而为”提供，重要数据请自行备份。

## 版本

- 0.3.0：迁移日志与断点恢复、回滚迁移、自测、应用图标与版本信息、Release 打包
- 0.2.0：空间体检、释放空间（休眠/清理/迁移）
- 0.1.0：磁盘布局只读查看

## 许可证

本项目代码使用 MIT License，见 [LICENSE](LICENSE)。

二进制分发包含 Qt 6.6.3（LGPLv3），声明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)，许可证文本见 `LICENSES/Qt-LICENSE.txt`。
