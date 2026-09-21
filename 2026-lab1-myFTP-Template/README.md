# myFTP 协议

> 本 Lab 允许使用 Vibe Coding，但提交时须附上完整的 Session 记录，以供查重。
>
> 2026 年课程将使用 xlab 平台进行实验发布、提交与评测。目前 xlab 平台的课程配置仍在准备中，访问方式和提交方法将在平台就绪后另行通知。DDL：`2026-10-25 23:59:59`（星期日）。
>
> 在 xlab 平台准备好之前，可以先克隆 [2026-lab1-myFTP-Template](https://github.com/N2Sys-EDU/2026-lab1-myFTP-Template)，在本地完成本 Lab 并运行测试。

MyFTP 是我们为了方便同学们快速理解 POSIX API 设计的一个简单的 Lab

在这个 Lab 中你需要完成一个简单的 FTP Server 和 FTP Client CLI (CLI指命令行界面)

## 1. 实现要求

1. MyFTP 的 Client 支持以下的命令
    1. `open <IP> <port>`: 建立一个到 `<IP>:<port>` 的连接
    2. `ls`: 获取对方当前工作目录下的文件列表。
    3. `cd <directory>`: 更改 Server 的工作目录，简单起见只需考虑`<directory>`为相对路径，若路径不存在则返回不存在。
    4. `get <filename>`: 将 Server 工作目录中的 `<filename>` 文件存放到 Client 工作目录的 `<filename>` 中。
    5. `put <filename>`: 将 Client 工作目录中的 `<filename>` 文件存放到 Server 工作目录的 `<filename>` 中。
    6. `sha256 <filename>`: 在 Server 工作目录中查询某个文件的 sha256 值，若不存在则返回不存在。
    7. `quit`: 如有连接则断开连接，回到 `open` 前的状态；如果已经是 `open` 前的状态，则关闭 Client。
2. MyFTP的Server需要支持如下的功能特点
    1. 获取文件列表。这里文件列表由指令 `ls` 生成，可以使用 `popen` 或者 `pipe+fork+execv` 的手段获取其他进程的输出结果。
    2. 更改工作目录。
    3. 下载文件。
    4. 上传文件。
    5. 支持 sha256 查询。这里校验和结果由指令 `sha256sum FILE` 生成，可以使用 `popen` 或者 `pipe+fork+execv` 的手段获取其他进程的输出结果。

### 1.1 Client的运行

基于“3. 如何进行本地测试”一节的描述，编译完成后，期望在 build 目录下生成可执行程序 `ftp_client`，你可以通过在 build 目录中直接执行 `./ftp_client` 启动 Client。  
实际测试时我们会通过在任意目录中使用 `ftp_client` 的绝对路径，在没有任何参数的情况下执行 `ftp_client`。

### 1.2 Server的运行

基于“3. 如何进行本地测试”一节的描述，编译完成后，期望在 build 目录下生成可执行程序 `ftp_server`，你可以通过在 build 目录中直接执行 `./ftp_server 127.0.0.1 12323` 启动 Server (该 Server 监听 127.0.0.1:12323)。  
实际测试时我们会通过在任意目录中使用 `ftp_server` 的绝对路径，以参数 IP 和 Port 启动 `ftp_server` 分别作为 Server 需要监听的 IP 和端口。

> 关于绝对路径和启动目录的说明  
> 假设我们的 `ftp_server` 可执行文件在 `/root/ftp/build/ftp_server` 目录下  
> 我们使用终端切换到目录 `/root/tmp` 目录下，`cd /root/tmp`  
> 然后使用绝对路径执行程序 `/root/ftp/build/ftp_server 127.0.0.1 20729`  
> 此时该指令就在 `/root/tmp` 目录下启动了 `ftp_server`

## 2. 技术规范

### 2.1 数据报文格式

``` cpp
struct {
    byte m_protocol[MAGIC_NUMBER_LENGTH]; /* protocol magic number (6 bytes) */
    type m_type;                          /* type (1 byte) */
    status m_status;                      /* status (1 byte) */
    uint32_t m_length;                    /* length (4 bytes) in Big endian*/
} __attribute__ ((packed));
```

注意我们添加`__attribute__ ((packed))`以避免数据结构对齐产生的问题

Client 和 Server 将交换协议信息，以执行各项功能。所有协议消息前面都有代码所示的协议头，基于 TCP 协议进行通信。  
即每一个 802.3 网络上的报文都有如下的形式:

```plain
|802.3 Header|IP Header|TCP Header|myFTP Header|myFTP Data|
```

其中 TCP Header 以及之前的头会由系统生成, 我们要实现的是一个应用层协议，也就是 myFTP Header 以及以后的内容。  

### 2.2 myFTP Header格式

文档中所称的请求为从 Client 发送给 Server 的报文，回复为从 Server 发送给 Client 的报文。  
对于所有的报文，myFTP Header 中的 `m_protocol` 字段的值都应当为 `"\xc1\xa1\x10ftp"` (即三个字符 '\xc1\xa1\x10' 后接字符串 "ftp" 共 6bytes)，我们通过这个字段来辨识是否为正确的协议版本即 myFTP 协议。  
`m_type` 字段标识了当前报文的类型，具体取值见 2.3。  
`m_status` 只会为 0 或者 1, Server 用该选项通知客户端当前状态，具体取值见 2.3。  
报文的长度 `m_length` 为**大端法**表示，长度包括包头和数据的**总长度**, 你可以认为在任何时候报文的长度不会超过 `limits.h` 中定义的 `INT_MAX` 。

### 2.3 功能描述

```mermaid
graph LR;
Start -- client start --> Idle;
Idle -- open connection --> Open_connection;
Open_connection -- connection rejected \n server is off --> Idle;
Open_connection -- connection accepted --> Main;
Main -- list files\n upload files\n download files\n sha256 files--> Main;
Main -- quit --> Idle
Idle -- quit --> End
```

上图描述了 Client 的状态机, Server 对于每一个 Client, 以及 Client 自身都应当维护这样一个简单的状态机。  
Client 在每一个时刻只能执行一个请求, 只有当一个请求执行完成后才能开始下一个。

#### 2.3.1 Open a Connection

Client 和 Server 会按照如下的步骤进行交互:

1. 当用户启动 Client 并输入以下命令时 `open SERVER_IP SERVER_PORT` (其中 `SERVER IP` 和 `SERVER_PORT` 分别是 Server 的 IP 地址和端口号)，那么一个 TCP 连接应该被连接到 Server 的 IP 地址和端口。
2. 接下来，Client 将向 Server 发送一个协议消息 `OPEN_CONN_REQUEST` ，请求打开一个连接。
3. 如果 Server 正在运行（即它正在监听 Server 的端口号），它将回复一个 `OPEN_CONN_REPLY` 类型的协议消息。
4. 最后 Client 收到 Server 的回复。

```mermaid
sequenceDiagram
    participant Client
    participant Server
    Note left of Client: open SERVER_IP SERVER_PORT
    Client -->> Server: OPEN_CONN_REQUEST
    Server -->> Client: OPEN_CONN_REPLY
    Note left of Client: if status=1, connection open
```

报文格式如下：

```c
// OPEN_CONN_REQUEST
struct {
    type = 0xA1
    status = unused
    length = 12
}
// OPEN_CONN_REPLY
struct {
    type = 0xA2
    status = 1
    length = 12
}
```

请注意：

1. 当 Server 启动时，他的 `OPEN_CONN_REQUEST` 类型的回复的 `m_status=1`
2. 当一个字段被说明为未使用时，无论是 Server 还是 Client 都不会读取该字段内容。

#### 2.3.2 List Files

用户在连接成功后，就可以执行主要功能。假设用户想列出存储在 Server 当前工作目录中的所有文件，用户将发出一个命令`ls`。然后，Client 将向 Server 发送一个协议消息，类型为 `LIST_REQUEST`。Server 将回复一个协议消息`LIST_REPLY` 以及可用文件的列表。  
所有文件都存储程序的工作目录中（注意工作目录不等价于可执行程序位置，而是从何处启动了该程序，上面有说明）。下面列出了对资源库目录的假设：

1. 该目录在 Server 启动前已被创建。
2. 当 Server 启动时，该目录可能包含文件。
3. 执行 `ls` 测试时，Server 的当前工作目录只包含常规文件，不包含子目录、符号链接等其他类型的目录项。
4. Server 进程有必要的权限来访问该目录以及该目录内的文件。
5. 文件名只包含以下字符：字母（a-z 和 A-Z）、数字（0-9）和英文句点（`.`）。
6. 在该目录下执行 `ls` 指令，其返回的结果的总长度不会超过 2047 个字符

要在Server端读取目录，你可以使用`popen`获取Linux中`ls`程序执行的返回结果，并将其结果返回，注意返回的内容结尾应当增加一个`\0`以表示内容结束。

```mermaid
sequenceDiagram
    participant Client
    participant Server
    Note left of Client: ls
    Client -->> Server: LIST_REQUEST
    Note right of Server: find the file list
    Server -->> Client: LIST_REPLY
    Note left of Client: display the file list
```

报文格式：

```c
// LIST_REQUEST
struct {
    type = 0xA3
    status = unused
    length = 12
}
// LIST_REPLY
struct {
    type = 0xA4
    status = unused
    length = 12 + strlen(payload) + 1
    payload = file names, as one null-terminated string
}
```

#### 2.3.3 Change the Directory

假设用户想更改 Server 当前的工作目录，用户将发出一个命令`cd DIR`，其中`DIR`为要切换的**相对路径**，并且只包含以下字符：字母（a-z和A-Z）和数字（0-9）。然后，Client 将向 Server 发送一个协议消息，类型为 `CHANGE_DIR_REQUEST`。  
如果路径不存在，Server 将回复一个协议消息 `CHANGE_DIR_REPLY`，`m_status` 设置为 0。  
如果路径存在，Server 将回复一个协议消息 `CHANGE_DIR_REPLY`，`m_status` 设置为 1，并将工作路径变更为`DIR`指定的**相对路径**。  

**提示**：C++17标准引入的标准库`<filesystem>`可以用于处理文件系统的目录和文件操作，包括检查目录是否存在、构建目录等等。

**注意**：同学们或许很容易想到用`chdir`函数来切换路径，但在任何一个服务线程中执行`chdir`都会导致整个 Server 进程的工作目录被修改，而我们希望 Client 侧的`cd`命令是独立的，即一个 Client 执行了`cd`不会导致其他 Client 视角下 Server 的工作路径发生了任何变化。

```mermaid
sequenceDiagram
    participant Client
    participant Server
    Note left of Client: cd DIR
    Client -->> Server: CHANGE_DIR_REQUEST
    Note right of Server: Check if directory exists.<br>If yes, reply status=1.<br>Else reply status=0.
    Server -->> Client: CHANGE_DIR_REPLY
    Note right of Server: If directory exists, change the directory accessed by this client
```

报文格式：

```c
// CHANGE_DIR_REQUEST
struct {
    type = 0xA5
    status = unused
    length = 12 + strlen(payload) + 1
    payload = one directory name, in one null-terminated string
}
// CHANGE_DIR_REPLY
struct {
    type = 0xA6
    status = 0 or 1
    length = 12
}
```

#### 2.3.4 Download Files

假设用户想从 Server 上下载一个文件，然后他发出了一个命令 `get FILE`， 其中 `FILE` 是要下载的文件的名称。  
然后，Client 将向 Server 发送一个协议消息 `GET_REQUEST`。Server 将首先检查该文件在其资源库目录中是否可用。  
如果文件不存在，Server 将回复一个协议消息 `GET_REPLY`，`m_status` 设置为 0。  
如果文件存在，Server 将回复一个协议消息 `GET_REPLY`，`m_status` 设置为 1。  
以及一个包含文件内容的 `FILE_DATA` 消息。  

请注意，一个文件可以是 ASCII 或二进制模式。你应该确保 ASCII 和二进制文件都被支持。此外，该程序将覆盖现有的本地文件。  
在这里，我们假设对于每个 Client，在用户发出下一个命令之前，一次只下载一个文件。下图展示了如何下载文件的消息流。

```mermaid
sequenceDiagram
    participant Client
    participant Server
    Note left of Client: get FILE
    Client -->> Server: GET_REQUEST
    Note right of Server: Check if file exists.<br>If yes, reply status=1.<br>Else reply status=0.
    Server -->> Client: GET_REPLY
    Note right of Server: File exists.
    Server -->> Client: FILE_DATA
```

报文格式：

```c
// GET_REQUEST
struct {
    type = 0xA7
    status = unused
    length = 12 + strlen(payload) + 1
    payload = one file name, in one null-terminated string
}
// GET_REPLY
struct {
    type = 0xA8
    status = 0 or 1
    length = 12
}
// FILE_DATA
struct {
    type = 0xFF
    status = unused
    length = 12 + file size
    payload = file payload
}
```

#### 2.3.5 Upload Files

假设用户想上传一个文件到 Server。然后，他发出一个命令 `put FILE` ,其中 `FILE` 是要上传的文件的名称。  
首先，Client 将检查该文件是否存在于本地（即在 Client）。如果不存在，Client 将显示一个错误消息，说明该文件不存在。  
如果文件在本地存在，那么 Client 将向 Server 发送一个协议消息 `PUT_REQUEST`。  
Server将回复一个协议消息 `PUT_REPLY` 并等待文件。然后，Client 将发送一个包含文件内容和`FILE_DATA` 消息。

这里，我们假设上传的文件位于 Client 进程的当前工作目录中。
另外，在用户发出下一个命令之前，每次只上传一个文件。  
最后，Server 将上传的文件存储在其工作目录下，使用用户提供的名称。  
注意，Server 可能会覆盖现有的文件。  
同样，所有文件可以是 ASCII 码，也可以是二进制。你应该确保二进制文件被支持。  
下图显示了如何上传一个文件的消息流。

```mermaid
sequenceDiagram
    participant Client
    participant Server
    Note left of Client: put FILE<br> if file exists
    Client -->> Server: PUT_REQUEST
    Server -->> Client: PUT_REPLY
    Client -->> Server: FILE_DATA
    Note right of Server: Store the file in repository.
```

报文格式：

```c
// PUT_REQUEST
struct {
    type = 0xA9
    status = unused
    length = 12 + strlen(payload) + 1
    payload = one file name, in one null-terminated string
}
// PUT_REPLY
struct {
    type = 0xAA
    status = unused
    length = 12
}
// FILE_DATA
struct {
    type = 0xFF
    status = unused
    length = 12 + file size
    payload = file payload
}
```

#### 2.3.6 Calculate the checksum

文件在传输的过程中可能发生错误，我们可以利用校验码来检查传输结果是否正常。

假设用户想查询 Server 上目录下某个文件的 sha256 校验值，他发出一个命令 `sha256 FILE`，其中 `FILE` 是想要查询校验码的文件名称。  
然后，Client 将向 Server 发送一个协议消息 `SHA_REQUEST`。Server 将首先检查该文件在其资源库目录中是否可用。  
如果文件不存在，Server 将回复一个协议消息 `SHA_REPLY`，`m_status` 设置为 0。
如果文件存在，Server 将回复一个协议消息 `SHA_REPLY`，`m_status` 设置为 1。
以及一个包含关于需要查询的文件的 sha256 校验码的 `FILE_DATA` 消息。  
如果文件存在，Client 需要将接收到的 sha256 校验码输出到屏幕。  
类似 ls 命令，要在 Server 端计算 sha256 校验码，你可以使用`popen`获取 Linux 中`sha256sum FILE` 程序执行的返回结果（其中 `FILE` 为需要查询的文件名），并将其结果返回，注意返回的内容结尾应当增加一个`\0`以表示内容结束。
下面是计算校验码的消息流。

```mermaid
sequenceDiagram
    participant Client
    participant Server
    Note left of Client: sha256 FILE
    Client -->> Server: SHA_REQUEST
    Note right of Server: Check if file exists.<br>If yes, reply status=1.<br>Else reply status=0.
    Server -->> Client: SHA_REPLY
    Note right of Server: File exists.
    Server -->> Client: FILE_DATA
```

报文格式：

```c
// SHA_REQUEST
struct {
    type = 0xAB
    status = unused
    length = 12 + strlen(payload) + 1
    payload = one file name, in one null-terminated string
}
// SHA_REPLY
struct {
    type = 0xAC
    status = 0 or 1
    length = 12
}
// FILE_DATA
struct {
    type = 0xFF
    status = unused
    length = 12 + strlen(payload) + 1
    payload = sha256 checksum, in one null-terminated string
}
```

#### 2.3.7 Close Connection

为了关闭连接，用户发出 `quit` 的命令。
Client 将向 Server 发送一个协议消息 `QUIT_REQUEST`。
Server 将回复一个协议消息 `QUIT_REPLY` 给 Client ，而 Client 应该释放连接（即关闭 TCP 连接）。此时进入状态机的 Idle 状态。  
再次输入 quit 命令，Client 程序将退出。

```c
// QUIT_REQUEST
struct {
    type = 0xAD
    status = unused
    length = 12
}
// QUIT_REPLY
struct {
    type = 0xAE
    status = unused
    length = 12
}
```

### 2.4 一个Client的Example

下图给出了一个执行Client的Example，你可以设计其他更炫的输出的Client，但是请注意，输入将从`STDIN`输入。

![Client Example](example.png)

### 2.5 关于报文格式的总的说明

下表对所有的报文格式作了一个汇总

|名称|type|status|length|payload|
|--|--|--|--|--|
|OPEN_CONN_REQUEST|0xA1|/|12|/|
|OPEN_CONN_REPLY|0xA2|1|12|/|
|LIST_REQUEST|0xA3|/|12|/|
|LIST_REPLY|0xA4|/|12 + strlen(payload) + 1|file names, as one null-terminated string|
|CHANGE_DIR_REQUEST|0xA5|/|12 + strlen(payload) + 1|one directory name, in one null-terminated string|
|CHANGE_DIR_REPLY|0xA6|0 or 1|12|/|
|GET_REQUEST|0xA7|/|12 + strlen(payload) + 1|one file name, in one null-terminated string|
|GET_REPLY|0xA8|0 or 1|12|/|
|FILE_DATA|0xFF|/|12 + file size|file payload|
|PUT_REQUEST|0xA9|/|12 + strlen(payload) + 1|one file name, in one null-terminated string|
|PUT_REPLY|0xAA|/|12|/|
|SHA_REQUEST|0xAB|/|12 + strlen(payload) + 1|one file name, in one null-terminated string|
|SHA_REPLY|0xAC|0 or 1|12|/|
|QUIT_REQUEST|0xAD|/|12|/|
|QUIT_REPLY|0xAE|/|12|/|

## 3. 如何进行本地测试

模板在 `test_local` 目录中提供了本地测试程序。
如果无法进行正常测试，请和助教联系。

### 3.1 获取仓库

在 xlab 平台准备好之前，可以执行以下命令克隆模板仓库：

```bash
git clone https://github.com/N2Sys-EDU/2026-lab1-myFTP-Template.git
cd 2026-lab1-myFTP-Template
```

如果使用 Lab0 中配置的 Docker 容器，建议在宿主机的挂载目录中克隆仓库，仅在容器内进行编译和测试，以免受到容器内用户配置或网络代理的影响。

### 3.2 编译 myFTP 程序

在项目根目录中执行：

```bash
cmake -B build
cmake --build build -j
```

我们期望此时在 `build` 目录下应该有两个可执行文件 `ftp_server` 和 `ftp_client` 分别是你编写的 Server 和 Client 程序。

### 3.3 编译测试程序

本地测试程序位于模板的 `test_local` 目录中。请在项目根目录（即同时包含顶层 `CMakeLists.txt` 和 `test_local` 目录的位置）执行以下命令进行编译：

```bash
cmake -S test_local -B test_local/build
cmake --build test_local/build -j
```

首次配置测试程序时，CMake 会从 GitHub 下载 GoogleTest，请确保当前环境能够访问 GitHub。

编译完成后，`test_local/build` 目录中会生成 `ftp_test`。在 x86-64 环境中，将测试程序和标准程序复制到主 `build` 目录：

```bash
cp test_local/ftp_client_std test_local/ftp_server_std test_local/build/ftp_test build/
```

如果在 ARM64 环境中直接编译和测试，请改用模板中的 ARM64 标准程序，并将它们重命名为测试程序所需的名称：

```bash
cp test_local/ftp_client_std_arm64 build/ftp_client_std
cp test_local/ftp_server_std_arm64 build/ftp_server_std
cp test_local/build/ftp_test build/
```

完成后，主 `build` 目录中应包含 `ftp_server`、`ftp_client`、`ftp_server_std`、`ftp_client_std` 和 `ftp_test`。

一个供参考的文件目录示意图如下：

```plain
CMakeLists.txt
ftp_server.cpp
ftp_client.cpp
build/
      ftp_server
      ftp_client
      ftp_server_std
      ftp_client_std
      ftp_test
      （其他构建文件）
test_local/
          CMakeLists.txt
          ftp_server_std
          ftp_client_std
          ftp_server_std_arm64
          ftp_client_std_arm64
          test.cpp
          build/
                ftp_test
                （其他构建文件）
          （其他文件）
（其他文件）
```

### 3.4 本地测试

在 `build` 目录中执行 `./ftp_test` 即可进行测试。

要求:

1. Server 和 Client 的可执行程序需要严格命名为 `ftp_server` 和 `ftp_client`。模板已经配置好对应的 CMake target，只需修改项目根目录中的 `ftp_server.cpp` 和 `ftp_client.cpp`。
2. 具体协议内容请参考技术规范一节实现,在进行某一项测试的时候,只有一侧会使用你的 Server/Client ,另一侧为标准程序,为了确保能够正常通信,我们希望你的代码能够和标准程序进行交互
3. 任何对提供的**二进制文件**的修改均被视为作弊行为, 一经发现该Lab将被记为0分

### 3.5 提交

xlab 平台的课程配置目前尚未完成。在平台准备好之前，只需在本地完成 Lab 并运行测试，无需提交。建议使用本地 Git 提交保存开发进度，但请勿尝试向 N2Sys-EDU 提供的模板仓库推送修改。

xlab 平台就绪后，我们会另行通知访问方式、提交方法和评测流程。DDL 为 `2026-10-25 23:59:59`（星期日）。

## 4. 分数计算

本次 Lab 总分 150 分

部分测试点会在截止时间前开放，另一部分测试点会在截止后统一进行测试；所有测试点的数据特点均会在下表中说明。xlab 平台开放后，同学们可以通过 xlab 提交并进行自动评测，截止时间前可见的测试满分为 100 分。

以下的表格给出了每一个测试点对应的 ID 和内容，你可以通过 `./ftp_test --gtest_filter=ID` 来只针对某个测试点执行测试。

或者使用通配符，如 `./ftp_test --gtest_filter="FTPServer.*"`，只针对 Server 进行测试。

每一个测试点名由`${类别}.${测试点名称}`构成

如没有特殊说明,则每一个测试点同时只有一个 Client

<table>
    <tr>
        <td>类别</td>
        <td>测试点名称</td>
        <td>测试内容</td>
        <td>分值</td>
        <td>是否在截止时间前开放</td>
        <td>数据点内容</td>
    </tr>
    <tr>
        <td rowspan="12">FTPServer</td>
        <td>Open</td>
        <td>测试OPEN_REQUEST</td>
        <td>10</td>
        <td>是</td>
        <td>收到正确的OPEN_REQUEST请求</td>
    </tr>
    <tr>
        <td>Get</td>
        <td>测试GET_REQUEST</td>
        <td>10</td>
        <td>是</td>
        <td>获取大小为 1–3 bytes、文件名长度为 5–7 bytes 的文件，文件名格式为随机整数加 <code>.txt</code>，内容随机生成</td>
    </tr>
    <tr>
        <td>Put</td>
        <td>测试PUT_REQUEST</td>
        <td>10</td>
        <td>是</td>
        <td>上传大小为 1–3 bytes、文件名长度为 5–7 bytes 的文件，文件名格式为随机整数加 <code>.txt</code>，内容随机生成</td>
    </tr>
    <tr>
        <td>List</td>
        <td>测试LIST_REQUEST</td>
        <td>10</td>
        <td>是</td>
        <td>获取文件夹内所有的文件列表</td>
    </tr>
    <tr>
        <td>CdGet</td>
        <td>测试CHANGE_DIR_REQUEST和GET_REQUEST</td>
        <td>10</td>
        <td>是</td>
        <td>更改 Server 的工作目录，并在新目录里获取大小为 1–3 bytes、文件名长度为 5–7 bytes 的文件，文件名格式为随机整数加 <code>.txt</code>，内容随机生成</td>
    </tr>
    <tr>
        <td>CdPut</td>
        <td>测试CHANGE_DIR_REQUEST和PUT_REQUEST</td>
        <td>10</td>
        <td>是</td>
        <td>更改 Server 的工作目录，并在新目录里上传大小为 1–3 bytes、文件名长度为 5–7 bytes 的文件，文件名格式为随机整数加 <code>.txt</code>，内容随机生成</td>
    </tr>
    <tr>
        <td>SHA256</td>
        <td>测试SHA_REQUEST</td>
        <td>10</td>
        <td>是</td>
        <td>获取文件的 sha256 校验码</td>
    </tr>
    <tr>
        <td>MultiGet</td>
        <td>测试多个 Clients 并发执行 GET_REQUEST</td>
        <td>0</td>
        <td>是</td>
        <td>多个 Clients 同时获取同一个文件，Client 数量不超过 16 个，用于辅助检查并发文件下载功能</td>
    </tr>
    <tr>
        <td>MultiPut</td>
        <td>测试多个 Clients 并发执行 PUT_REQUEST</td>
        <td>0</td>
        <td>是</td>
        <td>多个 Clients 同时上传不同文件，Client 数量不超过 16 个，用于辅助检查并发文件上传功能</td>
    </tr>
    <tr>
        <td>GetBig</td>
        <td>测试GET_REQUEST</td>
        <td>10</td>
        <td>否</td>
        <td>获取大小为 1 MB、文件名长度为 5–7 bytes 的文件，文件名格式为随机整数加 <code>.txt</code>，内容随机生成</td>
    </tr>
    <tr>
        <td>PutBig</td>
        <td>测试PUT_REQUEST</td>
        <td>10</td>
        <td>否</td>
        <td>上传大小为 1 MB、文件名长度为 5–7 bytes 的文件，文件名格式为随机整数加 <code>.txt</code>，内容随机生成</td>
    </tr>
    <tr>
        <td>MultiCdList</td>
        <td>测试CHANGE_DIR_REQUEST和LIST_REQUEST</td>
        <td>10</td>
        <td>否</td>
        <td>多个 Clients 分别进入不同的子目录，并获取对应目录的正确文件列表，Client 数量不超过 16 个</td>
    </tr>
    <tr>
        <td rowspan="5">FTPClient</td>
        <td>Open</td>
        <td>测试OPEN_REQUEST</td>
        <td>10</td>
        <td>是</td>
        <td>成功和Server建立连接</td>
    </tr>
    <tr>
        <td>Get</td>
        <td>测试GET_REQUEST</td>
        <td>10</td>
        <td>是</td>
        <td>获取大小为 1–3 bytes、文件名长度为 5–7 bytes 的文件，文件名格式为随机整数加 <code>.txt</code>，内容随机生成</td>
    </tr>
    <tr>
        <td>Put</td>
        <td>测试PUT_REQUEST</td>
        <td>10</td>
        <td>是</td>
        <td>上传大小为 1–3 bytes、文件名长度为 5–7 bytes 的文件，文件名格式为随机整数加 <code>.txt</code>，内容随机生成</td>
    </tr>
    <tr>
        <td>GetBig</td>
        <td>测试GET_REQUEST</td>
        <td>10</td>
        <td>否</td>
        <td>获取大小为 1 MB、文件名长度为 5–7 bytes 的文件，文件名格式为随机整数加 <code>.txt</code>，内容随机生成</td>
    </tr>
    <tr>
        <td>PutBig</td>
        <td>测试PUT_REQUEST</td>
        <td>10</td>
        <td>否</td>
        <td>上传大小为 1 MB、文件名长度为 5–7 bytes 的文件，文件名格式为随机整数加 <code>.txt</code>，内容随机生成</td>
    </tr>
</table>

## 5. 其他问题

> 文件名无空格等特殊符号，仅包含数字、字母和英文句点（`.`）
>
> 若无法执行 xxx_std 则请通过 `chmod +x xxx_std` 为该文件增加可执行权限
>
> 模板已经通过 `.gitignore` 忽略 `build` 目录。本地生成的可执行文件无需提交；xlab 的具体提交要求将在平台就绪后另行通知。
