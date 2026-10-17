### 测试点描述

以下的表格给出了每一个测试点对应的ID和内容，注意，lab2只测试sender/receiver一对一交互的情况。

<table>
    <tr>
        <td>测试点名称</td>
        <td>分值</td>
        <td>是否在Deadline前放出</td>
        <td>测试点内容</td>
    </tr>
    <tr>
        <td>NORMAL_HUGE_WINDOW</td>
        <td>10</td>
        <td>否</td>
        <td>使用回退N，窗口大小为20000，进行两次完整的无差错传输。第一次测试Sender，第二次测试Receiver，两次都通过才算通过测试</td>
    </tr>
    <tr>
        <td>NORMAL_OPT_HUGE_WINDOW</td>
        <td>10</td>
        <td>否</td>
        <td>使用选择重传，窗口大小为20000，进行两次完整的无差错传输。第一次测试Sender，第二次测试Receiver，两次都通过才算通过测试</td>
    </tr>
    <tr>
        <td>OPT_RECEIVER_MIXED_3</td>
        <td>10</td>
        <td>否</td>
        <td>使用选择重传 Receiver，窗口大小为32，故障率为60%，只进行一次完整的传输流程。四种传输错误均可能出现</td>
    </tr>
    <tr>
        <td>OPT_RECEIVER_MIXED_4</td>
        <td>10</td>
        <td>否</td>
        <td>使用选择重传 Receiver，窗口大小为32，故障率为50%，只进行一次完整的传输流程。可能出现丢包、损坏、乱序。</td>
    </tr>
    <tr>
        <td>OPT_SENDER_MIXED_3</td>
        <td>10</td>
        <td>否</td>
        <td>使用选择重传 Sender，窗口大小为32，故障率为60%，只进行一次完整的传输流程。四种传输错误均可能出现</td>
    </tr>
    <tr>
        <td>OPT_SENDER_MIXED_4</td>
        <td>10</td>
        <td>否</td>
        <td>使用选择重传 Sender，窗口大小为32，故障率为50%，只进行一次完整的传输流程。可能出现丢包、损坏、乱序。</td>
    </tr>
</table>