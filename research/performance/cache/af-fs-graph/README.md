# 202609\-AF\_FS统一流图表示

# 摘要

本文提出一种在同一张访问图上统一表示伪共享（False Sharing, FS）与访问亲和性（Affinity）的数据布局评价模型。模型先从动态访存记录中构造布局无关的逻辑访问图：节点表示稳定的逻辑数据元素，原始边权 $b_e$ 表示访问关系的统计强度。随后，对每个候选布局重新计算边两端的真实字节距离和缓存行关系，并把同一原始边权同时投影到 Affinity 正收益通道与 FS 负收益通道，得到布局相关的有符号净边权 $\omega_e(L)$。

该表示区分两类不应混淆的量：原始边权 $b_e$ 是观测证据，在不同布局下保持不变；净边权 $\omega_e(L)$ 是评价结果，会随布局改变。全文重点说明模型定义、构图必须采集的内容、统一权重公式及超参数含义，并以真实地址、字节距离和固定边权的四节点访问图，逐边计算紧凑布局、按行填充和转置布局的正负收益。

**关键词：**伪共享；访问亲和性；有符号访问图；数据布局；缓存行距离；统一边权；动态访存采样



# 总体架构





# 采样机 Sampler

## 输出的Entry的字段信息



|字段|含义|模型用途|
|---|---|---|
|object\_id|数组或数据对象身份|限定构图对象，避免无关地址被连接|
|**logical\_index**|多维下标、元素编号或字段编号|作为跨布局保持不变的节点身份|
|**address**|基准运行中的起始地址|核验地址映射和基准缓存行|
|access\_size|访问覆盖的字节数|构造真实字节区间和缓存行集合|
|core\_id|执行访问的物理核心 $c_i$|判断是否跨核|
|thread\_id|执行访问的软件线程 $t_i$|识别异线程共享与线程迁移|
|**memory\_mode**|读、写或原子读改写 $m_i$|判断访问对是否涉写|
|timestamp / sequence|时间戳或全局可比较顺序|构造有限窗口内的访问对|
|sampling\_probability|访问对进入样本的概率|采样率不一致时校正权重|

## 采样原理以及环境要求



暂略



# 生成器 Generator

## 前置知识

根据两个相关访问的核心关系、读写关系以及当前布局下是否位于同一缓存行，将边上的样本划分为四类：

① 异核访问，先前一次访问为写，并且两个逻辑元素位于同一 memory line。

② 异核访问，先前一次访问为写，但两个逻辑元素位于不同 memory line。

③ 异核访问，先前一次访问为读。

④ 同核访问。



只有①类样本具备形成典型伪共享的必要布局条件；

②类样本由于数据位于不同缓存行，不会因这两个逻辑对象之间的行粒度共享而形成伪共享；

③类不存在写失效竞争；

④类不存在跨核一致性竞争。

需要强调的是，①类并不等价于“已经发生了真/伪共享”，它仅表示 AF 采样中满足伪共享空间与访问条件的候选事件。



|类别|判定|解释|
|---|---|---|
|1|异核 \+ 异线程 \+ 先写 \+ 同缓存行|可由布局消除的候选伪共享暴露量，是必要条件，不是实测伪共享|
|2|异核 \+ 异线程 \+ 先写 \+ 异缓存行|不构成行级伪共享|
|3|异核 \+ 异线程 \+ 两端均读|只读共享，可贡献局部性|
|4|同核|无跨核竞争，可贡献局部性|



## 输出的图

生成器会根据分析目标的类型，自动生成三种不同维度的 DOT 图文件：
1. **单变量图 (graph.dot)**：包含零散的全局标量或独立变量（如 global_counter）。用于 package_simple 打包。
2. **结构体数组图 (graph.<array_name>.dot)**：保留数组下标信息（如 	eam[0].health）。用于交给 summarier 分析热/冷数组维度拆解。
3. **结构体抽象图 (graph.<StructName>.fields.dot)**：将结构体实例或结构体数组的访问抽象到“字段级别”（如 Point::x）。用于交给 package_fields 进行结构体内部缓存行布局重排。

格式

- 结点

- 边

    - **属性**：无方向

    - **权值**：二维向量 \(α,β\)

        - a，表示①类和②类访问

            - 只有α表示的访问会触发 **TS**/FS，β不会。在重排过程中，α和β的数值保持不变。对于两个结点之间的α，因为逻辑地址要么在同一memoryline上，要么不在，因此只能是①类与②类的一种。

            - 举个例子。如果两个结点原本在同一line上，重排后不在同一line上。它们之间边的权值a保持不变，但是a表示的访问类型由①变成了②。

        - β，表示③类和④类访问



## 定义边计算

假设有两节点 x、y，节点间权值 \(α, β\)，则：

**k**

数值上等于一次DDR访问用时减去一次L1C\-DATA命中用时**？**

**g**

数值上等于一次TS/FS访问延迟减去一次L1C\-DATA命中用时

**Logical Address是否在同一MemoryLine**

$isSameLine(x,y) = 0 \ or 1$

**边权代数化**

$ω = |(α, β)| = \dfrac{k \cdot (α+β)} γ $  , while \!isSameLine\(x,y\), otherwise: $ω = |(α, β)| = k \cdot β - g \cdot α $

变量处于不同行时：

γ \> 1 用于表征访问不同缓存行的变量，有概率引起的缓存行换入带来的延时，暂定 10

## 序列\-图 逻辑



> 记第 $i$ 条访问记录为 $r_i$，它访问逻辑元素 $x_i$。若 $r_j$ 在 $r_i$ 之后，并满足同一对象、同一阶段和时间窗口条件，则形成有向访问对：
> 
> $p_{ij}=(r_i,r_j),\qquad 0<t_j-t_i\le\tau$
> 
> 访问对 $p_{ij}$ 映射到边 $e=(x_i,x_j)$。同一个有序访问对只能生成一次，也只能映射到一条边。时间窗口 $\tau$ 越大，图能够观察更远的复用关系，但产生虚假亲和的风险也越高。
> 
> 



### 单个访问对的权重



$a_{i}^j=\exp\left(-\frac{\Delta t_{ij}}{\tau_w}\right) * (1 + 9 \cdot isSameLine(i,j))$

- $\Delta t_{ij}$：两次访问的时间差。

- $\tau_w$：时间衰减尺度，越小越强调短时间访问关系。

- 9：新加的，用于给同行的访问增加权重

权重 $a_{ij}$ 只描述访问关系本身的统计强度，必须对所有候选布局保持不变。

例如

```Plain Text
T0 R 0x400000 Word +8=20260915:15:16:17 Core0
T1 W 0x400002 Word +8=20260915:15:16:17 Core1
T2 W 0x400008 Word +8=20260915:15:16:18 Core2
```

- T0

    - 忽略第一条采样条目

- T1

    - 上一条操作是Read

    - 异核访问，先前一次访问为读，归位第三类

    - 对应 \(0x400000 , 0x400002 \) 的 $β\ +=\ a_{0x400000}^{0x400002}$ 

- T2

    - 上一条操作是Write

    - 异核访问，先前一次访问为写，并且两个逻辑元素位于同一 memory line。

    - 对应 \(0x400002 , 0x400008 \) 的 $α\ +=\ a_{0x400002}^{0x400008}$ 





# 推演机 Evolver

## 输出的图\(包含原结果\)



简要总结：根据不同维度N生成N个Layout 



# 总结机 Summarier



简要总结：分别计算每个Layout的Σ → 选择Σ最高的Layout → 给出建议或将Layout提交给应用机



![Image](https://internal-api-drive-stream.feishu.cn/space/api/box/stream/download/authcode/?code=N2UwNWU3MzJkMjU0ZmNkYTE4ZWZkNTI1OGJlMGZlZjVfNWRmMTQ3MmZkODQ1NzdkOTI0MzEwYjk3Y2UwZGNhYmFfSUQ6NzY4NTY2MjczNDA4NDUwODg2Ml8xNzkwOTE0ODgxOjE3OTEwMDEyODFfVjM)











$W_X = \dfrac{1}{(M-1)N} \sum_{y=0→N} \sum_{x=0→M-1} \omega_{y,x}^{y,x+1} = (\omega_{0,0}^{0,1} + \omega_{0,1}^{0,2} + \omega_{1,0}^{1,1} + ...)\cdot \dfrac 1 4$

# $W = W_X + W_Y $









# 打包机 Package Simple

## 功能简介
`package_simple` 专门用于对【非数组】的【离散单变量】进行缓存行打包（Cache-Line Packing）推演。
它读取由生成器生成的 `.dot` 流图，利用贪心聚合算法（Greedy Agglomerative Clustering）评估各变量间的正负收益，将亲和性高且不引发伪共享的变量智能打包进同一个 Cache Line 中，以获取最高的访存局部性收益。

## 使用方法
执行格式如下：
```bash
package_simple <dot_file> <alpha> <beta> <gamma> <cacheline_size>
```
**参数说明：**
- `dot_file`: `generator` 生成的包含单变量的 dot 拓扑图文件（注：数组会自动被跳过）。
- `alpha`, `beta`, `gamma`: 与 `summarier` 相同的权重超参数。
- `cacheline_size`: 目标系统缓存行大小（通常为 64）。

## 输出格式
程序会在原 `.dot` 文件相同的目录下，生成一个后缀替换为 `.layout.txt` 的文本文件（例如 `graph.dot` 会生成 `graph.layout.txt`）。
**每一行代表一个独立的 Cache Line 组合**。同行内的变量应当在物理内存中被连续相邻放置（或合并进同一个结构体中）。

**示例输出：**
```text
global_a global_b global_e
global_c global_d
global_f
```
这意味着建议将 `a, b, e` 放入第一个缓存行；`c, d` 放入第二个；`f` 单独成行。


# 结构体打包机 Package Fields

## 功能简介
`package_fields` 是针对 **结构体（Struct）内部缓存行重排** 定制的打包工具。与 `package_simple` 仅处理离散全局变量不同，`package_fields` 具备“主场识别”能力：
1. **处理 `.fields.dot`（独立结构体）**：自动提取特定前缀（如 `Point::`）的字段，无视图中的跨界干扰节点。

它最终输出每个 Cache Line 应当包含哪些字段，以此指导 C/C++ 代码中的 `__attribute__((aligned))` 和成员重排。

## 使用方法
```bash
package_fields <dot_file> <alpha> <beta> <gamma> <cacheline_size>
```
输出示例（`*.layout.txt`）：
```text
Target: Point
Cacheline size: 64
========================================
[Cacheline 0]
  - x
  - y
  - z
```

# 标准化编译流水线 (Makefile)

在完整的分析链路中，推荐使用如下标准的流水分发逻辑，将不同类型的图精准送给对应的分析机：

```makefile
opt:
	rm -rf *.dot *.txt *.layout.txt
	$(affspath)/generator/target/debug/generator -i *.csv -o graph.dot --binary t*.exe --disable-self-weight
	@for file in graph.*.dot; do \
		case "$$file" in \
			*.fields.dot) \
				echo "👉 [Struct Packing] $$file ..."; \
				$(affspath)/package_fields/target/debug/package_fields "$$file" 1 1 10 64; \
				;; \
			graph.dot) \
				;; \
			*) \
				echo "👉 [Array Summarizing] $$file ..."; \
				$(affspath)/summarier/target/debug/summarier "$$file" 1 1 10 64; \
				;; \
		esac; \
	done
	@if [ -f graph.dot ]; then \
		echo "👉 [Globals Packing] graph.dot ..."; \
		$(affspath)/package_simple/target/debug/package_simple graph.dot 1 1 10 64; \
	fi
```
