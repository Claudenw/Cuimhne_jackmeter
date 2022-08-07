dpkg --add-architecture armhf
apt-get update
apt-get -y install gcc-arm-linux-gnueabihf libjack-jackd2-dev:armhf automake cmake debhelper
export PKG_CONFIG_PATH=/usr/lib/arm-linux-gnueabihf/pkgconfig
export PKG_CONFIG=/usr/bin/pkg-config

export DEB_BUILD_OPTIONS=nostrip
cd /src
mkdir -p build-scripts
aclocal
autoheader
automake --add-missing --copy
autoconf
dh clean
fakeroot dh binary-arch
mkdir -p target
mv ../cuimhne-jackmeter*.deb target
