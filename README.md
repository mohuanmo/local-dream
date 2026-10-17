<div align="center">

# Local Dream <img src="./assets/icon.png" width="32" alt="Local Dream">

**在 Android 上本地跑 Stable Diffusion：骁龙 NPU / 麒麟 NPU / Vulkan GPU / CPU 多后端可选**

_Android Stable Diffusion with Snapdragon NPU acceleration. Kirin NPU, Vulkan GPU and CPU inference are also supported._

<img src="./assets/demo1.jpg" alt="App Demo" width="800">

</div>

## 关于这个仓库

本项目基于 [xororz/local-dream](https://github.com/xororz/local-dream) **大幅改造**而来，并非简单 fork。原项目的整体架构、推理流程与基础功能由原作者完成；本版本在此基础上做了大范围重写与扩展，其中相当一部分代码修改（后端接入、参数面板、机型兼容修补、构建脚本与文档整理）由 AI 辅助完成，再由我逐项验证、调参与取舍。

> [!NOTE]
> 上游原有的 NPU 支持范围依然适用：SD1.5 需要 Hexagon V68 及以上的骁龙 NPU，SDXL 需要骁龙 8 Gen3 及以上机型。本版本新增的麒麟 NPU、Vulkan 后端等支持，请以下方「使用前提」为准。

## 这一版主要加了什么

| 改动 | 说明 |
| --- | --- |
| **Vulkan（VK）后端** | 新增 Vulkan 推理路径，让没有可用 NPU 的机型也能走 GPU 加速。**兼容性有限**，详见下方「使用前提」。 |
| **骁龙纹理优化** | 针对高通 Adreno 的纹理路径做专门处理，减少纹理格式转换与上传开销。**需预留较大内存**，详见下方「使用前提」。 |
| **MNN 自定义算子优化强度** | 加载期预优化做成可选枚举，自己决定 MNN 在加载时替每个算子挑多激进的实现方案。 |
| **32 位 / 16 位模型加载切换** | 可在 FP32 与 FP16 权重加载之间切换：16 位省内存、跑得快，32 位精度更稳，老设备与新芯片各取所需。 |
| **麒麟 NPU 支持** | 新增华为麒麟平台 NPU 推理，**仅限较新的麒麟机型**。 |
| **模型存储路径不再受 Android 版本限制** | 去掉了原先修改模型目录时的系统版本判断，旧版本 Android 上也能自定义模型存储路径。 |
| **运行时内存占用优化** | 重做模型加载与张量生命周期管理，降低**运行时的峰值内存**。注意：这不改变模型文件大小，也不减少存储占用。 |

## 算子优化强度是什么

「算子优化强度」是一个**枚举档位**，作用点在**加载期**，不是出图时的参数。

MNN 在加载模型时会先做一轮预优化：为每个算子挑选该机型上最合适的实现方案（计算核、算法路径），之后整轮推理都沿用这次选定的结果。这个档位决定的就是这一轮预优化「做多少」。

目前一共三档：

| 档位 | MNN 做的事 | 实际感受 |
| --- | --- | --- |
| **极速** | 最激进的预优化，尽量给每个算子都挑最快的实现 | 出图最快；代价是加载更久、加载阶段内存占用更高，个别机型或模型可能异常 |
| **默认** | 常规预优化 | 加载与出图比较平衡，日常就用这档 |
| **慢** | 只做最基本的算子选择 | 加载最快、加载时内存占用最小，但出图会明显慢一档；适合内存紧张或排查问题时用 |

三点需要注意：

- 档位名说的是**出图速度**，不是加载速度。选「极速」换来的是每张图更快，但打开模型那一下更久。
- 改档位**不会立刻生效**，要重新加载模型（或重启 App）才会重新走一遍预优化。
- 它和「32 位 / 16 位加载」不是一回事：后者改的是权重精度，前者改的是算子实现的选择策略。

## ⚠️ 使用前提与已知限制

- **VK 后端并非所有机型都能使用。** 它依赖设备厂商的 Vulkan 驱动，兼容性因机型而异：有的机器能正常跑，有的速度反而比 CPU 还慢，还有的会直接崩溃。建议先保存当前工作，再用小分辨率试跑；出问题就切回 NPU 或 CPU，不要把它当成默认后端。
- **骁龙纹理优化占用内存较多。** 它是**整块一次性申请**内存，不是按需零碎分配，所以要的是一块足够大的连续内存。开启前请确认可用内存比较充裕（大内存机型更合适），内存紧张的机器开了很可能申请失败或直接因内存不足崩溃。
- **麒麟 NPU 仅限较新的麒麟设备。** 老麒麟平台（早期 Kirin 9 系及更早）不在支持范围内，即使装上也会回落到 CPU。
- **内存占用优化只作用于运行时。** 它降低的是推理过程中的峰值内存，模型文件本身多大还是多大，存储占用不会因此变小。
- **32 位 / 16 位切换是加载策略，不是画质开关。** 16 位更快、更省内存，但可能出现细节偏差；32 位更稳，内存压力也更大。

## 使用说明

操作指南与文档见 [Guide Site](https://ld-guide.chino.icu)。上游的功能说明（模型列表、构建步骤等）同样适用于本版本。

## 致谢

感谢 [xororz](https://github.com/xororz) 及 Local Dream 原项目，以及以下开源项目：

- **[Qualcomm QNN SDK](https://www.qualcomm.com/developer/software/qualcomm-ai-engine-direct-sdk)** - NPU 模型执行
- **[alibaba/MNN](https://github.com/alibaba/MNN/)** - CPU / GPU 模型执行
- **[happyyzy/stable-diffusion.cpp](https://github.com/happyyzy/stable-diffusion.cpp)** - 端侧 DiT 推理性能优化
- **[xtensor-stack](https://github.com/xtensor-stack)** - 张量运算与调度
- **[mlc-ai/tokenizers-cpp](https://github.com/mlc-ai/tokenizers-cpp)** - 文本分词
- **[yhirose/cpp-httplib](https://github.com/yhirose/cpp-httplib)**、**[facebook/zstd](https://github.com/facebook/zstd)**、**[nlohmann/json](https://github.com/nlohmann/json)**、**[nothings/stb](https://github.com/nothings/stb)** - 网络、压缩、序列化与图像处理
- **[CompVis/stable-diffusion](https://github.com/CompVis/stable-diffusion)** 及所有模型作者
- **[xinntao/Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN)**、**[Kim2091/UltraSharpV2](https://huggingface.co/Kim2091/UltraSharpV2)** - 图像放大
- **[bhky/opennsfw2](https://github.com/bhky/opennsfw2)** - 内容过滤
- **AOSP、Material Design、Jetpack Compose** 及 **[coil-kt/coil](https://github.com/coil-kt/coil)** 等 Android 组件

## 赞助原作者

下面是赞助原作者的渠道（爱发电上的赞助方案也在其中）。如果条件允许，请支持原作者：

<a href="https://ko-fi.com/xororz">
    <img height="36" style="border:0px;height:36px;" src="https://storage.ko-fi.com/cdn/kofi2.png?v=3" border="0" alt="Buy Me a Coffee at ko-fi.com" />
</a>
<a href="https://afdian.com/a/xororz">
    <img height="36" style="border-radius:12px;height:36px;" src="https://pic1.afdiancdn.com/static/img/welcome/button-sponsorme.jpg" alt="在爱发电支持我" />
</a>

---

## English

**Local Dream · mohuanmo build** — Run Stable Diffusion locally on Android with selectable Snapdragon NPU, Kirin NPU, Vulkan GPU and CPU backends, fully offline.

Heavily modified from [xororz/local-dream](https://github.com/xororz/local-dream), not a light fork. A significant part of the code changes in this build (backend integration, settings panels, device compatibility fixes, build scripts and docs) was produced with AI assistance, then verified, tuned and curated by me.

**What's new**

- **Vulkan backend** — a GPU path for devices without a usable NPU. Compatibility is limited: on some devices it is slower than CPU, and on others it crashes outright.
- **Snapdragon (Adreno) texture path optimizations** — less texture conversion and upload overhead. Requires one large contiguous memory allocation, so it needs plenty of free RAM.
- **Configurable MNN operator-optimization level** — a load-time enum with three settings: **Fastest** (most aggressive pre-optimization: fastest generation, slower load and higher load-time memory), **Default** (balanced), **Slow** (minimal kernel selection: fastest load, smallest memory, clearly slower generation). Takes effect after the model is reloaded.
- **FP32 / FP16 weight loading switch** — trade memory and speed against precision.
- **Kirin NPU support** — newer Kirin devices only.
- **Model storage path no longer tied to the Android version** — the system-version check on changing the model directory is gone.
- **Runtime memory footprint reductions** — lower peak RAM during inference; model file size and storage usage are unchanged.

Upstream scope still applies: SD1.5 needs Hexagon V68 or newer Snapdragon NPUs; SDXL needs Snapdragon 8 Gen 3 or newer. See [Guide Site](https://ld-guide.chino.icu) for usage docs.

Credits to the original Local Dream project and the MNN, QNN, stable-diffusion.cpp, Real-ESRGAN and opennsfw2 authors.

If you can, please support the upstream author (Ko-fi / Afdian).
