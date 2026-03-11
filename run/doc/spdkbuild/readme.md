1375  uname -m
 1376  lscpu
 1377  cd ../spdk
 1378  ./configure --target-arch=armv8-a
 1379  rm -rf /home/nvdia/working/wwx/spdk/dpdk/build-tmp
 1380  ./configure --target-arch=armv8-a
 1381  make