# Python Code Develop PCDOS

PCDOS 是基于 BuildRoot 配置的最小 Linux 系统，仅包含运行 Python 所相关的动态库。  
PCDOS 在 BuildRoot 的基础上，修改了 getty 程序，使其可以将终端重定向到 App 内。  
PCDOS 在 BuildRoot 基础上新增的软件包代码采用 GPL 协议开源，未修改的原始代码按 BuildRoot 中原始软件包所规定的开源协议开源。

## 编译 RootFS
> 目前 PCDOS 使用的自动发布流水线是运行 ARM Ubuntu 24.04 的 Apple Mac Mini M4 设备。维护者会保证在与流水线相同的操作系统环境中可以正常编译。  
> M4 标准版真的很快，同样的代码在 14 核 E5 上全量编译需约 ~5h，在 M4 上只需要 ~1h :)

产出 rootfs.tar 的完整编译脚本如下：
```
rm -rf output            # 删除缓存 (大部分时候增量编译的结果有问题)
make -j10                # 编译和安装基本 BuildRoot
chmod +x postbuild.sh    # 给 postbuild.sh 加执行权限
./postbuild.sh           # 编译和安装 getty 等修改的软件包
```
目前 PCDOS 缺少 GNU-ToolChain 来支持其编译自身，也就是说您无法在 iPad 中直接完成源码编译，请使用独立的 Linux ARM 或 X86 系统编译 PCDOS。  
我们很乐于看到您为 PCDOS 支持完整编译工具链提交的 PR，相关修改可能会在 Python Code Develop 的新版本中发布。

## 替代 Python Code Develop 中的 PCDOS
1. 确保 Python Code Develop 已关闭。
2. 从 CI 或 Linux 开发机上下载打包产物 (rootfs.tar) 到设备上，移动到文件 App 中 Python Code Develop 目录下，和 rootfs 文件夹平级。
3. 删除 rootfs 文件夹，然后点击 rootfs.tar 来解压缩，这应该会直接生成 rootfs 目录 (也有可能直接解压缩到当前目录下了，如果是这样请新建文件夹调整):
4. 完成操作后，保证您的目录结构如下，如果不对，请调整：
```
Python Code Develop
|-- rootfs
    |-- bin
    |-- dev
    |-- usr
    |-- ...
```
5. 重新启动 Python Code Develop，就会加载新的软件啦！
