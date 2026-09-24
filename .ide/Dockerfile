# .ide/Dockerfile —— CNB 云开发环境预装 qemu-sim 依赖（官方机制: custom-dev-env.md）
#
# 为什么: 默认镜像 cnbcool/default-dev-env 不含 qemu, 旧做法是 run.sh 首次跑时
#   apt-get install（每次环境重建都可能重装, ~90s + 依赖 apt 源可达）。
# 效果: 环境创建阶段构建本镜像 ⇒ 进环境即有 qemu-system-arm/unsquashfs/cpio,
#   run.sh 的 step1 直接跳过, setup 从 ~90s 降到秒级。
# 回退: 本 Dockerfile 构建失败时, CNB 自动回退 default-dev-env（官方保证）,
#   且 run.sh 的 step1 仍有 apt 兜底 ⇒ 本文件最坏情况无副作用。
#
# 预装清单（qemu-sim/ENVIRONMENT.md §1, 全部 --no-install-recommends）:
#   qemu-system-arm(需 virt 机器 + virtio-gpu 支持, Debian 13 实测 10.0.13 可用)
#   squashfs-tools(unsquashfs, 解 rootfs.sqsh 与 modloop)  cpio(组 initramfs)

FROM cnbcool/default-dev-env:latest

RUN set -e \
 && apt-get update -qq \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
      qemu-system-arm squashfs-tools cpio gzip \
 && rm -rf /var/lib/apt/lists/*
