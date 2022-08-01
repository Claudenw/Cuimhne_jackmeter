dpkg --add-architecture armhf
apt-get update
apt-get -y install gcc-arm-linux-gnueabihf libjack-jackd2-dev:armhf automake cmake
export PKG_CONFIG_PATH=/usr/lib/arm-linux-gnueabihf/pkgconfig
export PKG_CONFIG=/usr/bin/pkg-config

cd /src
mkdir -p build-scripts
aclocal
autoheader
automake --add-missing --copy
autoconf
./configure --host arm-linux-gnueabihf  --disable-dependency-tracking
make clean all
