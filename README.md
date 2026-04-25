# UAssetRead - UE Commandlet 资产数据导出工具

## 概述

通过 UE Commandlet 读取 `.uasset` 文件，导出结构化数据（JSON/YAML），用于资产分析、文档生成、CI/CD 流水线等场景。

---

## 使用指南

### 前置条件

1. 将插件放置在项目的 `Plugins/UAssetRead/` 目录中并完成编译
2. 在 UE 编辑器的插件管理页面（Edit → Plugins）确认 **UAssetRead** 已启用

---

### 方式一：Commandlet（批量命令行导出）

Commandlet 在无 UI 的命令行模式下运行，适合批量导出和 CI/CD 流水线。

**可执行文件（UE 5.4）：**
```
{引擎根目录}\Engine\Binaries\Win64\UnrealEditor-Cmd.exe
```

**调用格式：**
```bat
UnrealEditor-Cmd.exe "<项目>.uproject" -run=AssetExport [参数]
```

#### 导出单个资产

```bat
UnrealEditor-Cmd.exe "S:\Project\UEProject\Empty54\Empty54.uproject" ^
  -run=AssetExport ^
  -AssetPath="/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter" ^
  -OutputDir="D:\Export"
```

输出文件：`D:\Export\Game\ThirdPerson\Blueprints\BP_ThirdPersonCharacter.json`

#### 批量导出整个目录（递归）

```bat
UnrealEditor-Cmd.exe "S:\Project\UEProject\Empty54\Empty54.uproject" ^
  -run=AssetExport ^
  -AssetPath="/Game/" ^
  -OutputDir="D:\Export" ^
  -Recursive
```

> `-AssetPath` 填目录时必须以 `/` 结尾；填单个资产时不加 `/`。

#### 只导出指定类型（按类型过滤）

```bat
UnrealEditor-Cmd.exe "S:\Project\UEProject\Empty54\Empty54.uproject" ^
  -run=AssetExport ^
  -AssetPath="/Game/" ^
  -OutputDir="D:\Export" ^
  -Recursive ^
  -Filter=Blueprint,DataTable
```

#### 导出为 YAML 格式

```bat
UnrealEditor-Cmd.exe "S:\Project\UEProject\Empty54\Empty54.uproject" ^
  -run=AssetExport ^
  -AssetPath="/Game/Materials/" ^
  -OutputDir="D:\Export" ^
  -Format=yaml
```

#### 输出到标准输出（供 Agent / 脚本直接消费）

加 `-stdout` 参数后，JSON 不写入文件，而是打印到 stdout，并用固定分隔符包裹，方便在日志噪声中可靠提取。

```bat
UnrealEditor-Cmd.exe "S:\Project\UEProject\Empty54\Empty54.uproject" ^
  -run=AssetExport ^
  -AssetPath="/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter" ^
  -stdout ^
  -NoLogTimes -unattended -nosplash
```

> `-NoLogTimes -unattended -nosplash` 可大幅减少 UE 日志噪声，建议 `-stdout` 模式下始终附加。

**stdout 输出格式：**

```
<<<ASSET_DUMP_BEGIN>>>
{"assetPath":"/Game/.../BP_ThirdPersonCharacter",...}
<<<ASSET_DUMP_END>>>
```

多个资产时每行一个 JSON（JSONL）：

```
<<<ASSET_DUMP_BEGIN>>>
{"assetPath":"/Game/A",...}
{"assetPath":"/Game/B",...}
<<<ASSET_DUMP_END>>>
```

**Agent 端提取规则（正则）：**

```python
import re, json

output = subprocess.check_output(["UnrealEditor-Cmd.exe", ...], text=True)
m = re.search(r'<<<ASSET_DUMP_BEGIN>>>\n(.*?)<<<ASSET_DUMP_END>>>', output, re.DOTALL)
if m:
    records = [json.loads(line) for line in m.group(1).splitlines() if line.strip()]
```

```javascript
const m = output.match(/<<<ASSET_DUMP_BEGIN>>>\n([\s\S]*?)<<<ASSET_DUMP_END>>>/);
const records = m[1].trim().split('\n').map(l => JSON.parse(l));
```

#### 全部参数

| 参数 | 类型 | 说明 | 默认值 |
|------|------|------|--------|
| `-AssetPath=` | 字符串 | 单个资产包路径或目录路径（目录须以 `/` 结尾） | **必填** |
| `-OutputDir=` | 字符串 | 输出根目录，文件按资产路径镜像存放（`-stdout` 时忽略） | `{项目}/Saved/AssetExport` |
| `-Format=` | `json` \| `yaml` | 输出格式（`-stdout` 时始终为 JSON） | `json` |
| `-Recursive` | 开关 | 批量模式下递归子目录 | 关闭 |
| `-Filter=` | 逗号分隔 | 按资产类名过滤，如 `Blueprint,DataTable,StaticMesh` | 全部类型 |
| `-stdout` | 开关 | 输出到 stdout（JSONL + 分隔符），不写文件 | 关闭 |

#### 输出文件结构

输出文件在 `OutputDir` 下按资产路径镜像存放：

```
OutputDir/
└── Game/
    ├── Characters/
    │   ├── BP_Hero.json
    │   └── BP_Enemy.json
    ├── DataTables/
    │   └── DT_Items.json
    └── Materials/
        └── M_Character.json
```

---

### 方式二：编辑器内右键菜单（导出到剪贴板）

在 UE 编辑器内容浏览器中，对任意资产**右键** → **Asset Actions** → **Export To JSON**。

操作完成后，该资产的完整 JSON 内容已复制到系统剪贴板，直接粘贴即可使用。

> 适合快速查看单个资产的导出结果，无需指定输出目录和重新编译。

---

## 需求总览

| # | 资产类型 | 导出内容 |
|---|---------|---------|
| 1 | 普通蓝图 (Blueprint) | 属性列表、函数列表、函数实现（节点+连接关系） |
| 2 | 所有蓝图 | 父类继承链（一直到 C++ 层） |
| 3 | 实现接口的蓝图 | 接口路径列表 |
| 4 | Actor 子类 | 组件层级树 |
| 5 | UDataTable / UDataAsset | 数据内容导出为 JSON/YAML |
| 6 | StaticMesh / SkeletalMesh | 顶点数、三角面数、LOD数、材质槽、BoundingBox、Skeleton引用、Collision |
| 7 | 音频 / 贴图 | 资产类型、文件大小、音频时长、贴图尺寸 |
| 8 | 材质 (Material) | 材质节点列表 + 连接关系 |
| 9 | UMG (Widget Blueprint) | Widget 层级树（类型、名称、尺寸、锚点、子节点） |

---

## 详细需求

### 1. 普通蓝图对象

导出蓝图自身定义的属性和函数（包括实现的接口函数）。函数实现使用统一 Graph Schema（见下方 [图数据统一 Schema](#图数据统一-schema-blueprint--material)）。

```json
{
  "assetPath": "/Game/Characters/PlayerCharacter",
  "assetType": "Blueprint",
  "properties": [
    { "type": "FString", "name": "PlayerTag", "category": "Default", "defaultValue": "" },
    { "type": "float", "name": "PlayerDistance", "category": "Default", "defaultValue": "0.0" }
  ],
  "functions": [
    { "name": "GetName", "access": "Public", "isOverride": false, "isPure": true,
      "inputs": [],
      "outputs": [{ "type": "FString", "name": "ReturnValue" }]
    },
    { "name": "SetDistance", "access": "Public", "isOverride": false, "isPure": false,
      "inputs": [{ "type": "float", "name": "NewDistance" }],
      "outputs": []
    }
  ],
  "graphs": [
    { "graph_name": "EventGraph", "graph_type": "Ubergraph", "nodes": ["..."], "edges": ["..."] },
    { "graph_name": "GetName", "graph_type": "Function", "nodes": ["..."], "edges": ["..."] },
    { "graph_name": "SetDistance", "graph_type": "Function", "nodes": ["..."], "edges": ["..."] }
  ]
}
```

### 2. 父类继承链

从当前类一直追溯到 C++ 原生类为止。

```json
{
  "parents": [
    "/Game/Base/CharacterBase",
    "ACharacter"
  ]
}
```

- 蓝图父类：保存完整资产路径 `/Game/...`
- C++ 父类：保存类名（无路径前缀），作为继承链终点

### 3. 接口列表

```json
{
  "interfaces": [
    "/Game/Interfaces/BPI_Interactable",
    "/Game/Interfaces/BPI_Damageable"
  ]
}
```

### 4. 组件层级（Actor 子类）

当蓝图父类链中包含 `AActor` 时，导出组件层级树。

```json
{
  "components": {
    "name": "DefaultSceneRoot",
    "type": "USceneComponent",
    "children": [
      {
        "name": "Mesh",
        "type": "UStaticMeshComponent",
        "children": []
      },
      {
        "name": "NS_Effect",
        "type": "UNiagaraComponent",
        "children": []
      }
    ]
  }
}
```

### 5. 数据资产 (UDataTable / UDataAsset)

- **UDataTable**: 按行导出，每行 RowName + 所有字段值
- **UDataAsset**: 导出所有 UPROPERTY 字段

```json
{
  "assetType": "DataTable",
  "rowStruct": "FItemData",
  "rows": [
    { "RowName": "Item_001", "Name": "Sword", "Damage": 50, "Weight": 3.5 },
    { "RowName": "Item_002", "Name": "Shield", "Damage": 0, "Weight": 8.0 }
  ]
}
```

### 6. Mesh 资产

```json
{
  "assetType": "StaticMesh",
  "vertexCount": 12450,
  "triangleCount": 8200,
  "lodCount": 3,
  "lods": [
    { "lod": 0, "vertexCount": 12450, "triangleCount": 8200 },
    { "lod": 1, "vertexCount": 3000, "triangleCount": 2000 },
    { "lod": 2, "vertexCount": 800, "triangleCount": 500 }
  ],
  "materialSlots": [
    { "index": 0, "name": "Body", "materialPath": "/Game/Materials/M_Body" },
    { "index": 1, "name": "Eyes", "materialPath": "/Game/Materials/M_Eyes" }
  ],
  "boundingBox": {
    "min": { "x": -50.0, "y": -50.0, "z": 0.0 },
    "max": { "x": 50.0, "y": 50.0, "z": 180.0 },
    "size": { "x": 100.0, "y": 100.0, "z": 180.0 }
  },
  "skeletonRef": "/Game/Characters/Skeleton/SK_Mannequin",
  "collision": {
    "hasSimpleCollision": true,
    "hasComplexCollision": true,
    "simpleCollisionType": "UseSimpleAsComplex",
    "collisionPrimitives": [
      { "type": "Box", "center": { "x": 0, "y": 0, "z": 90 }, "extent": { "x": 50, "y": 50, "z": 90 } }
    ]
  }
}
```

> `skeletonRef` 仅 SkeletalMesh 有效。

### 7. 音频 / 贴图

```json
{
  "assetType": "SoundWave",
  "sizeBytes": 2048000,
  "duration": 12.5,
  "sampleRate": 44100,
  "numChannels": 2
}
```

```json
{
  "assetType": "Texture2D",
  "sizeBytes": 4194304,
  "width": 2048,
  "height": 2048,
  "format": "DXT5",
  "hasMips": true,
  "mipCount": 12
}
```

### 8. 材质

导出材质表达式节点 + 连接关系。使用统一 Graph Schema（`graph_type: "Material"`），材质最终输出引脚（BaseColor、Normal 等）作为 Material Result 节点的 input pins。

```json
{
  "assetType": "Material",
  "graphs": [
    {
      "graph_name": "MaterialGraph",
      "graph_type": "Material",
      "nodes": [
        {
          "node_id": "A1B2C3D4-...",
          "node_class": "MaterialExpressionTextureSample",
          "title": "BaseColor_Tex",
          "pos": [-400, 0],
          "pins": [
            { "pin_id": "P001", "name": "UVs", "direction": "input", "pin_type": { "category": "float2" }, "links": [] },
            { "pin_id": "P002", "name": "RGB", "direction": "output", "pin_type": { "category": "float3" }, "links": ["P010"] },
            { "pin_id": "P003", "name": "A", "direction": "output", "pin_type": { "category": "float" }, "links": [] }
          ],
          "extra": { "texture": "/Game/Textures/T_Base" }
        },
        {
          "node_id": "B2C3D4E5-...",
          "node_class": "MaterialExpressionMultiply",
          "title": "Multiply",
          "pos": [-200, 0],
          "pins": [
            { "pin_id": "P010", "name": "A", "direction": "input", "pin_type": { "category": "float3" }, "links": ["P002"] },
            { "pin_id": "P011", "name": "B", "direction": "input", "pin_type": { "category": "float3" }, "links": ["P020"] },
            { "pin_id": "P012", "name": "Result", "direction": "output", "pin_type": { "category": "float3" }, "links": ["P030"] }
          ],
          "extra": {}
        },
        {
          "node_id": "C3D4E5F6-...",
          "node_class": "MaterialExpressionConstant3Vector",
          "title": "TintColor",
          "pos": [-400, 200],
          "pins": [
            { "pin_id": "P020", "name": "RGB", "direction": "output", "pin_type": { "category": "float3" }, "links": ["P011"] }
          ],
          "extra": { "value": { "r": 1.0, "g": 0.8, "b": 0.6 } }
        },
        {
          "node_id": "D4E5F6A7-...",
          "node_class": "MaterialResultNode",
          "title": "Material Attributes",
          "pos": [0, 0],
          "pins": [
            { "pin_id": "P030", "name": "BaseColor", "direction": "input", "pin_type": { "category": "float3" }, "links": ["P012"] },
            { "pin_id": "P031", "name": "Normal", "direction": "input", "pin_type": { "category": "float3" }, "links": [] },
            { "pin_id": "P032", "name": "Roughness", "direction": "input", "pin_type": { "category": "float" }, "links": [] }
          ],
          "extra": {}
        }
      ],
      "edges": [
        { "from_node": "A1B2C3D4-...", "from_pin": "P002", "to_node": "B2C3D4E5-...", "to_pin": "P010", "is_exec": false },
        { "from_node": "C3D4E5F6-...", "from_pin": "P020", "to_node": "B2C3D4E5-...", "to_pin": "P011", "is_exec": false },
        { "from_node": "B2C3D4E5-...", "from_pin": "P012", "to_node": "D4E5F6A7-...", "to_pin": "P030", "is_exec": false }
      ]
    }
  ]
}
```

### 9. UMG Widget Blueprint

导出 Widget 层级树。

```json
{
  "assetType": "WidgetBlueprint",
  "widgetTree": {
    "type": "UCanvasPanel",
    "name": "RootPanel",
    "slot": { "sizeX": 1920, "sizeY": 1080, "anchorMinX": 0.0, "anchorMinY": 0.0, "anchorMaxX": 1.0, "anchorMaxY": 1.0 },
    "children": [
      {
        "type": "UButton",
        "name": "btnClick",
        "slot": { "sizeX": 200, "sizeY": 60, "anchorMinX": 0.5, "anchorMinY": 0.5 },
        "children": [
          {
            "type": "UTextBlock",
            "name": "txtBtnLabel",
            "slot": { "sizeX": 200, "sizeY": 60 },
            "children": [],
            "text": "Click Me"
          }
        ]
      },
      {
        "type": "UTextBlock",
        "name": "txtPlayerName",
        "slot": { "sizeX": 300, "sizeY": 40, "anchorMinX": 0.0, "anchorMinY": 0.0 },
        "children": [],
        "text": ""
      }
    ]
  }
}
```

> UMG 蓝图同时也会导出 需求1~3 的蓝图信息（属性、函数、继承链、接口）。

---

## 图数据统一 Schema (Blueprint & Material)

蓝图节点图和材质节点图均使用统一 Graph Schema。核心原则：**节点和连线分离，靠 GUID 引用**。蓝图图是 DAG（执行流 + 数据流两套图共存），不是树。

### 顶层结构 (Graph)

```json
{
  "graph_name": "EventGraph",
  "graph_type": "Ubergraph",
  "nodes": [ "..." ],
  "edges": [ "..." ]
}
```

| graph_type | 说明 |
|------------|------|
| `Ubergraph` | 事件图（EventGraph，可多个） |
| `Function` | 函数图 |
| `Macro` | 宏图 |
| `AnimGraph` | 动画蓝图状态机 |
| `Material` | 材质表达式图 |

### Node 结构

```json
{
  "node_id": "A1B2C3D4-...",
  "node_class": "K2Node_IfThenElse",
  "title": "Branch",
  "comment": "玩家死亡判断",
  "pos": [320, 128],
  "pins": [ "..." ],
  "extra": {}
}
```

- `node_id`: `FGuid`，全图唯一
- `node_class`: UE 类名，决定语义
- `title`: 编辑器显示名
- `comment`: 节点注释（可选）
- `pos`: 编辑器坐标 `[x, y]`
- `extra`: 节点专属数据（不同节点类型不同，见下方示例）

### Pin 结构

```json
{
  "pin_id": "E5F6...",
  "name": "Condition",
  "display_name": "Condition",
  "direction": "input",
  "pin_type": {
    "category": "bool",
    "sub_category": "",
    "sub_category_object": "/Script/Engine.Actor",
    "container": "none",
    "is_reference": false,
    "is_const": false
  },
  "default_value": "true",
  "links": ["对端 pin_id 1", "对端 pin_id 2"]
}
```

- `pin_id`: `FGuid`，全图唯一
- `direction`: `input` | `output`
- `pin_type.category`: `exec` | `bool` | `int` | `float` | `object` | `struct` | `float2` | `float3` | ...
  - `exec` 表示执行流引脚，其余为数据流
- `container`: `none` | `array` | `set` | `map`
- `default_value`: 未连线时字面值（字符串化）
- `links`: 对端 pin_id 列表（一对多）

### Edge 结构（可选冗余表）

遍历 `pin.links` 足以还原连线。Edge 表为扁平冗余表示，方便全图分析/可视化工具直接消费。

```json
{
  "from_node": "节点GUID",
  "from_pin": "PinGUID",
  "to_node": "节点GUID",
  "to_pin": "PinGUID",
  "is_exec": true
}
```

### 蓝图节点示例

#### 函数调用 (K2Node_CallFunction)

```json
{
  "node_id": "...",
  "node_class": "K2Node_CallFunction",
  "title": "Set Distance",
  "pos": [480, 200],
  "pins": [
    { "pin_id": "...", "name": "execute", "direction": "input", "pin_type": { "category": "exec" }, "links": ["..."] },
    { "pin_id": "...", "name": "self", "direction": "input", "pin_type": { "category": "object", "sub_category_object": "/Script/Engine.Actor" }, "links": [] },
    { "pin_id": "...", "name": "NewDistance", "direction": "input", "pin_type": { "category": "float" }, "default_value": "100.0", "links": [] },
    { "pin_id": "...", "name": "then", "direction": "output", "pin_type": { "category": "exec" }, "links": ["..."] }
  ],
  "extra": {
    "function_name": "SetDistance",
    "function_owner": "/Game/Base/CharacterBase.CharacterBase_C",
    "is_pure": false,
    "is_latent": false
  }
}
```

#### 执行序列 (K2Node_ExecutionSequence)

```json
{
  "node_id": "...",
  "node_class": "K2Node_ExecutionSequence",
  "title": "Sequence",
  "pos": [200, 100],
  "pins": [
    { "pin_id": "...", "name": "execute", "direction": "input", "pin_type": { "category": "exec" }, "links": ["..."] },
    { "pin_id": "...", "name": "then_0", "direction": "output", "pin_type": { "category": "exec" }, "links": ["..."] },
    { "pin_id": "...", "name": "then_1", "direction": "output", "pin_type": { "category": "exec" }, "links": ["..."] },
    { "pin_id": "...", "name": "then_2", "direction": "output", "pin_type": { "category": "exec" }, "links": ["..."] }
  ],
  "extra": { "pin_count": 3 }
}
```

#### 变量获取 (K2Node_VariableGet)

```json
{
  "node_id": "...",
  "node_class": "K2Node_VariableGet",
  "title": "Get PlayerTag",
  "pos": [100, 300],
  "pins": [
    { "pin_id": "...", "name": "PlayerTag", "direction": "output", "pin_type": { "category": "string" }, "links": ["..."] }
  ],
  "extra": {
    "variable_name": "PlayerTag",
    "variable_class": "Self"
  }
}
```

### 材质节点 extra 示例

| node_class | extra 内容 |
|------------|------------|
| `MaterialExpressionTextureSample` | `{ "texture": "/Game/Textures/T_Base" }` |
| `MaterialExpressionConstant` | `{ "value": 0.5 }` |
| `MaterialExpressionConstant3Vector` | `{ "value": { "r": 1.0, "g": 0.8, "b": 0.6 } }` |
| `MaterialExpressionScalarParameter` | `{ "parameter_name": "Roughness", "default_value": 0.5, "group": "Surface" }` |
| `MaterialResultNode` | `{}` (pins 即 BaseColor/Normal/Roughness 等最终输出槽) |

---

## 技术架构

### Commandlet 入口

```
UAssetExportCommandlet : UCommandlet
```

**调用方式：**
```bat
UnrealEditor-Cmd.exe "<项目>.uproject" -run=AssetExport -AssetPath="/Game/Characters/PlayerCharacter" -OutputDir="D:/Export" -Format=json
```

**参数：**

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-AssetPath` | 单个资产路径 或 目录路径（批量） | 必填 |
| `-OutputDir` | 输出目录 | `{ProjectDir}/Saved/AssetExport` |
| `-Format` | `json` 或 `yaml` | `json` |
| `-Recursive` | 批量模式是否递归子目录 | `true` |
| `-Filter` | 资产类型过滤 (如 `Blueprint,DataTable`) | 全部 |

### 模块结构

```
UAssetRead/
├── Source/
│   └── UAssetRead/
│       ├── UAssetRead.Build.cs
│       ├── Public/
│       │   ├── UAssetExportCommandlet.h        # Commandlet 入口
│       │   ├── Exporters/
│       │   │   ├── IAssetExporter.h             # 导出器接口
│       │   │   ├── FBlueprintExporter.h         # 蓝图导出
│       │   │   ├── FDataAssetExporter.h         # 数据资产导出
│       │   │   ├── FMeshExporter.h              # Mesh 导出
│       │   │   ├── FMediaExporter.h             # 音频/贴图导出
│       │   │   ├── FMaterialExporter.h          # 材质导出
│       │   │   └── FWidgetExporter.h            # UMG 导出
│       │   └── Utils/
│       │       ├── FBlueprintGraphUtils.h       # 蓝图节点图解析
│       │       └── FJsonYamlWriter.h            # JSON/YAML 序列化
│       └── Private/
│           ├── UAssetExportCommandlet.cpp
│           ├── Exporters/
│           │   ├── FBlueprintExporter.cpp
│           │   ├── FDataAssetExporter.cpp
│           │   ├── FMeshExporter.cpp
│           │   ├── FMediaExporter.cpp
│           │   ├── FMaterialExporter.cpp
│           │   └── FWidgetExporter.cpp
│           └── Utils/
│               ├── FBlueprintGraphUtils.cpp
│               └── FJsonYamlWriter.cpp
├── UAssetRead.uplugin
└── README.md
```

### 核心流程

```
Commandlet::Main()
  ├── 解析命令行参数
  ├── 收集资产列表 (AssetRegistry)
  ├── 遍历每个资产：
  │     ├── LoadObject / LoadPackage
  │     ├── 判断资产类型 → 选择对应 Exporter
  │     ├── Exporter::Export(UObject*) → TSharedPtr<FJsonObject>
  │     └── FJsonYamlWriter::Write(JsonObj, OutputPath, Format)
  └── 输出统计摘要
```

### 类型判断逻辑

```cpp
if (UBlueprint* BP = Cast<UBlueprint>(Asset))
{
    if (BP->ParentClass->IsChildOf(AActor::StaticClass()))
        // 导出组件 (需求4)
    if (UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Asset))
        // 额外导出 Widget 层级 (需求9)
    // 导出属性、函数、继承链、接口 (需求1,2,3)
}
else if (UDataTable* DT = Cast<UDataTable>(Asset))
    // 需求5
else if (UDataAsset* DA = Cast<UDataAsset>(Asset))
    // 需求5
else if (UStaticMesh* SM = Cast<UStaticMesh>(Asset))
    // 需求6
else if (USkeletalMesh* SKM = Cast<USkeletalMesh>(Asset))
    // 需求6
else if (USoundWave* Sound = Cast<USoundWave>(Asset))
    // 需求7
else if (UTexture2D* Tex = Cast<UTexture2D>(Asset))
    // 需求7
else if (UMaterial* Mat = Cast<UMaterial>(Asset))
    // 需求8
else if (UMaterialInstance* MI = Cast<UMaterialInstance>(Asset))
    // 需求8 (材质实例参数)
```

---

## 关键实现细节

### 蓝图属性读取

```cpp
// 遍历 UBlueprint::NewVariables
for (const FBPVariableDescription& Var : Blueprint->NewVariables)
{
    // Var.VarName, Var.VarType (FEdGraphPinType), Var.Category, Var.DefaultValue
}
```

### 蓝图函数读取

```cpp
// 遍历 FunctionGraphs
for (UEdGraph* Graph : Blueprint->FunctionGraphs)
{
    // Graph->GetFName() = 函数名
    // Graph->Nodes = 节点列表
}
```

### 蓝图节点 + 连接关系

```cpp
for (UEdGraphNode* Node : Graph->Nodes)
{
    // Node->GetClass()->GetName() = 节点类型
    // Node->GetNodeTitle(ENodeTitleType::FullTitle) = 标题
    for (UEdGraphPin* Pin : Node->Pins)
    {
        // Pin->Direction (Input/Output)
        // Pin->LinkedTo = 连接的 Pin 列表
        for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
        {
            // LinkedPin->GetOwningNode() = 目标节点
        }
    }
}
```

### 继承链

```cpp
TArray<FString> Parents;
UClass* Current = Blueprint->ParentClass;
while (Current)
{
    if (Current->ClassGeneratedBy) // 蓝图类
        Parents.Add(Current->ClassGeneratedBy->GetPathName());
    else // C++ 类 → 终点
    {
        Parents.Add(Current->GetName());
        break;
    }
    Current = Current->GetSuperClass();
}
```

### 接口列表

```cpp
for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
{
    // Interface.Interface->GetPathName()
}
```

### 组件层级

```cpp
// 从 SimpleConstructionScript 获取组件树
if (USCS_Node* RootNode = Blueprint->SimpleConstructionScript->GetDefaultSceneRootNode())
{
    // 递归遍历 RootNode->GetChildNodes()
    // RootNode->ComponentClass->GetName() = 类型
    // RootNode->GetVariableName() = 名称
}
```

### UMG Widget 层级

```cpp
UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Blueprint);
UWidget* RootWidget = WBP->WidgetTree->RootWidget;
// 递归遍历:
if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
{
    for (int32 i = 0; i < Panel->GetChildrenCount(); i++)
        // Panel->GetChildAt(i)
}
// Slot 信息: Widget->Slot → UCanvasPanelSlot 获取 Position/Size/Anchors
```

### 材质节点

```cpp
UMaterial* Material = ...;
for (UMaterialExpression* Expr : Material->GetExpressions())
{
    // Expr->GetClass()->GetName() = 节点类型
    // Expr->Desc = 描述
    // 遍历 Inputs/Outputs 获取连接
}
```

---

## Build.cs 依赖模块

```csharp
PublicDependencyModuleNames.AddRange(new string[]
{
    "Core",
    "CoreUObject",
    "Engine",
    "UnrealEd",        // Commandlet, UBlueprint
    "BlueprintGraph",  // K2Node 类型
    "KismetCompiler",  // 蓝图编译相关
    "UMG",             // Widget 相关
    "Slate",
    "SlateCore",
    "RenderCore",      // Mesh 信息
    "RHI",             // 渲染资源
    "Json",            // JSON 序列化
    "JsonUtilities",
});
```

---

## 输出文件命名

输出路径按资产路径镜像：

```
OutputDir/
├── Game/
│   ├── Characters/
│   │   └── PlayerCharacter.json
│   ├── DataTables/
│   │   └── DT_Items.json
│   └── UI/
│       └── WBP_MainHUD.json
```

---

## TODO / 后续扩展

- [ ] 支持 YAML 输出（引入第三方 yaml-cpp 或自行实现简易 YAML Writer）
- [ ] 支持 Level 资产导出（Actor 列表、Transform、引用关系）
- [ ] 支持 AnimBlueprint 动画蓝图（状态机、Blend节点）
- [ ] 支持 Niagara 系统导出
- [ ] 支持增量导出（基于资产修改时间）
- [ ] 支持并行导出（多线程加载+序列化）
- [ ] 添加资产依赖关系图导出
- [ ] Web 可视化工具读取导出 JSON

---

## 版本记录

| 版本 | 日期 | 内容 |
|------|------|------|
| 0.1 | 2026-04-25 | 初始需求文档 |
| 0.2 | 2026-04-25 | 统一 Graph Schema：节点+连线分离，GUID 引用，DAG 结构 |
