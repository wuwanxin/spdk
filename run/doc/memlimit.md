问题：mem内存限制，导致DPDK无法正常工作
sudo scripts/setup.sh
"nvdia" user memlock limit: 64 MB

This is the maximum amount of memory you will be
able to use with DPDK and VFIO if run as user "nvdia".
To change this, please adjust limits.conf memlock limit for user "nvdia".



sudo vim /etc/security/limits.conf
在文件末尾添加以下内容：
HwHiAiUser   soft    memlock         unlimited
HwHiAiUser   hard    memlock         unlimited
注销用户后重新登录，或者重启系统以使更改生效。

sudo scripts/setup.sh
[sudo] password for nvdia: 
INFO: Requested 1024 hugepages but 1024 already allocated 