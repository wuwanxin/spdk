# 发现可用的NQN
sudo nvme discover --transport=tcp --traddr=172.18.1.243 --trsvcid=4420

# 连接到Target
# 替换 NQN 为你的 target_config.json 中定义的 nqn.2016-06.io.spdk:cnode1
sudo nvme connect --transport=tcp --traddr=172.18.1.243 --trsvcid=4420 --nqn="nqn.2016-06.io.spdk:cnode1"

# 连接后，你应该能看到一个新的 nvme 设备，例如 /dev/nvme0n1
# 可以用 lsblk 或 fdisk -l 检查
lsblk



# 1. 尝试加载 nvme-tcp 模块
sudo modprobe nvme-tcp

# 2. 确认模块是否加载成功 (可选，但推荐)
lsmod | grep nvme_tcp

# 预期输出类似：
# nvme_tcp               28672  0
# nvme                   65536  2 nvme_tcp,nvme_core

# 3. 确认 /dev/nvme-fabrics 文件是否存在 (可选，但推荐)
ls -l /dev/nvme-fabrics

# 预期输出类似：
# crw------- 1 root root 246, 0 Dec  4 18:00 /dev/nvme-fabrics

# 4. 再次尝试发现 NVMe-oF Target
sudo nvme discover --transport=tcp --traddr=172.18.1.243 --trsvcid=4420
sudo nvme discover --transport=tcp --traddr=172.18.1.243 --trsvcid=4420

Discovery Log Number of Records 1, Generation counter 1
=====Discovery Log Entry 0======
trtype:  tcp
adrfam:  ipv4
subtype: nvme subsystem
treq:    not required
portid:  0
trsvcid: 4420
subnqn:  nqn.2016-06.io.spdk:cnode1
traddr:  172.18.1.243
sectype: none


sudo nvme connect --transport=tcp --traddr=172.18.1.243 --trsvcid=4420 --nqn="nqn.2016-06.io.spdk:cnode1"
sudo nvme connect --transport=tcp --traddr=172.18.1.243 --trsvcid=4420 --nqn="nqn.2016-06.io.spdk:cnode1"
(base) quadra@nuhd-enc:~/working/wuwx/JetPack/sdk/JetPack_6.2.1_Linux/Linux_for_Tegra/source/src_out/kernel_src_build$ lsblk
NAME        MAJ:MIN RM   SIZE RO TYPE MOUNTPOINTS
loop0         7:0    0     4K  1 loop /snap/bare/5
loop1         7:1    0    74M  1 loop /snap/core22/2163
loop2         7:2    0  63.8M  1 loop /snap/core20/2669
loop3         7:3    0 250.1M  1 loop /snap/firefox/7355
loop4         7:4    0  73.9M  1 loop /snap/core22/2139
loop5         7:5    0  63.8M  1 loop /snap/core20/2682
loop6         7:6    0 250.6M  1 loop /snap/firefox/7423
loop7         7:7    0 400.8M  1 loop /snap/gnome-3-38-2004/112
loop8         7:8    0 349.7M  1 loop /snap/gnome-3-38-2004/143
loop9         7:9    0   516M  1 loop /snap/gnome-42-2204/202
loop10        7:10   0 516.2M  1 loop /snap/gnome-42-2204/226
loop11        7:11   0  91.7M  1 loop /snap/gtk-common-themes/1535
loop12        7:12   0  12.9M  1 loop /snap/snap-store/1113
loop13        7:13   0  12.2M  1 loop /snap/snap-store/1216
loop14        7:14   0  50.8M  1 loop /snap/snapd/25202
loop15        7:15   0  50.9M  1 loop /snap/snapd/25577
loop16        7:16   0   568K  1 loop /snap/snapd-desktop-integration/253
loop17        7:17   0   576K  1 loop /snap/snapd-desktop-integration/315
sda           8:0    0   3.6T  0 disk
└─sda1        8:1    0   3.6T  0 part /data
sdb           8:16   0   3.6T  0 disk
├─sdb1        8:17   0   128M  0 part
└─sdb2        8:18   0   3.6T  0 part
sr0          11:0    1  1024M  0 rom
nvme0n1     259:0    0   7.8T  0 disk
nvme1n1     259:1    0 476.9G  0 disk
├─nvme1n1p1 259:2    0   512M  0 part /boot/efi
└─nvme1n1p2 259:3    0 476.4G  0 part /var/snap/firefox/common/host-hunspell
                                      /
nvme2n1     259:5    0   512M  0 disk
(base) quadra@nuhd-enc:~/working/wuwx/JetPack/sdk/JetPack_6.2.1_Linux/Linux_for_Tegra/source/src_out/kernel_src_build$
