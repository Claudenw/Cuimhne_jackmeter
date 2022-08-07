dpkg --add-architecture armhf
apt-get update
apt-get -y install gcc-arm-linux-gnueabihf libjack-jackd2-dev:armhf automake cmake debhelper build-essential crossbuild-essential-armhf
export PKG_CONFIG_PATH=/usr/lib/arm-linux-gnueabihf/pkgconfig
export PKG_CONFIG=/usr/bin/pkg-config
export CC=/usr/bin/arm-linux-gnueabihf-gcc
export DEB_BUILD_OPTIONS=nostrip
cd /src
mkdir -p build-scripts
aclocal
autoheader
automake --add-missing --copy
autoconf
dh clean
dpkg-buildpackage -aarmhf -B
mkdir -p debian/artifacts
ls -l ..
find .. -name cuimhne-jackmeter?\* -type f
mv ../cuimhne-jackmeter_* debian/artifacts
