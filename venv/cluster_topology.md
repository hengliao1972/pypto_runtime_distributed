# Linqu Virtual Cluster Topology — 1024-Node Configuration

## Physical Topology

```
L6 (CLOS2 — 全集群):  1 实例, 包含 1024 个主机
 └── L5 (CLOS1 — 超节点):  16 个, 每个 64 主机
      └── L4 (POD — 服务器柜/机框):  4 个/超节点, 每个 16 主机
           └── L3 (HOST — 单 OS 实例):  16 个/POD
                └── L2 (CHIP):  1 chip/host (MockChipBackend)
                     └── L0 (CORE_GROUP):  模拟
```

### 数量计算

| Level | 名称 | 每上级包含 | 全局数量 | 索引范围 |
|-------|------|-----------|---------|---------|
| L6 | CLOS2 (Cluster-level-2) | — | 1 | l6=0 |
| L5 | CLOS1 (Cluster-level-1) | 16 per L6 | 16 | l5=0..15 |
| L4 | POD (Cluster-level-0) | 4 per L5 | 64 | l4=0..3 |
| L3 | HOST (Node) | 16 per L4 | 1024 | l3=0..15 |
| L2 | CHIP | 1 per L3 | 1024 | l2=0 |
| L0 | CORE_GROUP | mock | mock | l0=0 |

### 带宽梯度

| 层间 | 连接类型 | 模拟方式 |
|------|---------|---------|
| L0–L2 (Core↔Chip) | 片上互连 | MockChipBackend (同进程内存) |
| L2–L3 (Chip↔Host) | PCIe/NVLink | 同进程调用 |
| L3–L4 (Host↔Pod, 16 hosts) | 高带宽 InfiniBand/NVLink | Unix Domain Socket |
| L4–L5 (Pod↔CLOS1, 4 pods = 64 hosts) | 高带宽 fat-tree spine | Unix Domain Socket |
| L5–L6 (CLOS1↔CLOS2, 16 supernodes = 1024 hosts) | 收缩带宽 | Unix Domain Socket |

## 坐标映射

每个节点的坐标由 4 级索引唯一确定:

```
(l6=0, l5=S, l4=P, l3=H)

其中:
  S ∈ [0, 15]   — 超节点编号
  P ∈ [0, 3]    — 超节点内的 Pod 编号
  H ∈ [0, 15]   — Pod 内的主机编号
```

**全局主机编号 (flat index):**
```
global_idx = S × 64 + P × 16 + H
global_idx ∈ [0, 1023]
```

**IP 映射 (模拟):**
```
IP = 10.<S>.<P>.<H>
例: 节点 (l5=3, l4=2, l3=7) → IP = 10.3.2.7
```

## 文件系统布局

```
/tmp/linqu/<system_name>/
├── L6_0/
│   ├── L5_0/
│   │   ├── L4_0/
│   │   │   ├── L3_0/
│   │   │   │   ├── daemon.sock
│   │   │   │   ├── code_cache/
│   │   │   │   ├── data_cache/
│   │   │   │   └── logs/
│   │   │   ├── L3_1/
│   │   │   │   └── ...
│   │   │   ├── ...
│   │   │   └── L3_15/
│   │   ├── L4_1/
│   │   │   ├── L3_0/ ... L3_15/
│   │   ├── L4_2/
│   │   │   ├── L3_0/ ... L3_15/
│   │   └── L4_3/
│   │       ├── L3_0/ ... L3_15/
│   ├── L5_1/
│   │   └── (same structure)
│   ├── ...
│   └── L5_15/
│       └── (same structure)
└── orchestrator.log
```

总目录数: 1024 个 L3 节点目录, 每个 3 个子目录 = **4096 个目录**.

## 进程资源估算 (单机运行 1024 进程)

| 资源 | 每进程 | 1024 进程合计 |
|------|--------|-------------|
| 虚拟内存 | ~10 MB (daemon idle) | ~10 GB |
| 常驻内存 (RSS) | ~2 MB | ~2 GB |
| 文件描述符 | ~10 (socket + files) | ~10,240 |
| Unix Socket 文件 | 1 | 1,024 |
| 线程 | 2 (main + listen) | 2,048 |

**系统要求:**
- `ulimit -n 65536` (文件描述符上限)
- `ulimit -u 8192` (进程数上限)
- `/tmp` 至少 1 GB 可用空间
- 建议 16 GB+ RAM 的 arm64 机器
