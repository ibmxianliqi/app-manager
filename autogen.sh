#!/bin/bash
################################################################################
## This script is used to install all 3rd-party dependency libraries
################################################################################

mkdir -p dep
cd dep
export ROOTDIR=`pwd`

if [ "$(id -u)" != "0" ]; then
    echo "This script must be run as root"
    exit 1
fi

GCC_VER_GTE49=$(echo `gcc -dumpversion | cut -f1-2 -d.` \>= 4.9 | bc)
if [ $GCC_VER_GTE49 != "1"]; then
	# GCC 4.9.4
	cd $ROOTDIR
	wget http://www.netgull.com/gcc/releases/gcc-4.9.4/gcc-4.9.4.tar.gz
	tar zxvf gcc-4.9.4.tar.gz
	cd gcc-4.9.4
	./contrib/download_prerequisites
	mkdir build
	cd build
	../configure --enable-checking=release --enable-languages=c,c++ --disable-multilib
	make -j 4
	make install
	gcc -v
	\cp -f /usr/local/lib64/libstdc++.so.6* /usr/lib64
fi

CMAKE=
if [ -f "/usr/bin/yum" ]; then
	# RHEL
	yum install -y epel-release
	yum install -y https://centos7.iuscommunity.org/ius-release.rpm

	yum install -y git222 make cmake3 gcc-c++
	CMAKE=$(which cmake3)
	yum install -y dos2unix openssl-devel

	# https://www.cnblogs.com/fujinzhou/p/5735578.html
	yum install -y ruby rubygems ruby-devel
	yum install -y rpm-build
elif [ -f "/usr/bin/apt" ]; then
	#Ubuntu
	CMAKE=$(which cmake)
	apt install -y dos2unix g++ git make zlib1g-dev libssl-dev cmake alien
	apt install -y libboost-all-dev libace-dev 
	#apt install -y libcpprest-dev liblog4cpp5-dev
	apt install -y ruby ruby-dev rubygems
fi

#install fpm
gem install fpm

# build boost_1_69_0 on RHEL
if [ -f "/usr/bin/yum" ]; then
	# BOOST:
	# https://www.cnblogs.com/eagle6688/p/5840773.html
	wget https://dl.bintray.com/boostorg/release/1.69.0/source/boost_1_69_0.tar.gz
	tar zxvf boost_1_69_0.tar.gz
	cd ./boost_1_69_0
	./bootstrap.sh
	./b2
	./b2 install --prefix=/usr/local/boost169
	ls -al /usr/local/boost169/lib*/libboost_system.so.1.69.0
	cd $ROOTDIR
fi

# cpprestsdk (use -DBUILD_SHARED_LIBS=0 for static link):
# need export BOOST_LIBRARYDIR & BOOST_INCLUDEDIR before build
# https://stackoverflow.com/questions/49877907/cpp-rest-sdk-in-centos-7
# build fix https://github.com/microsoft/cpprestsdk/blob/04917da60935b7f48e178ddd1c84b55546f9aa40/Release/src/json/json_parsing.cpp
git clone --branch 9d8f544001cb74544de6dc8c565592f7e2626d6e https://github.com/microsoft/cpprestsdk.git cpprestsdk
cd cpprestsdk
git submodule update --init
cd Release
$CMAKE .. -DCMAKE_BUILD_TYPE=Release -DBOOST_ROOT=/usr/local/boost169 -DBUILD_SHARED_LIBS=1 -DCMAKE_CXX_FLAGS="-Wno-error=cast-align -Wno-error=conversion -Wno-error=missing-field-initializers"
make
make install
ls -al /usr/local/lib*/libcpprest.so
cd $ROOTDIR


# build log4cpp:
# https://my.oschina.net/u/1983790/blog/1587568
wget https://jaist.dl.sourceforge.net/project/log4cpp/log4cpp-1.1.x%20%28new%29/log4cpp-1.1/log4cpp-1.1.3.tar.gz
tar zxvf log4cpp-1.1.3.tar.gz
cd log4cpp/
./autogen.sh
./configure
make
make install
ls -al /usr/local/lib*/liblog4cpp.a
cd $ROOTDIR
	
# build ACE on RHEL
if [ -f "/usr/bin/yum" ]; then
	# ubuntu does not need build ACE
	# ACE:
	# https://www.cnblogs.com/tanzi-888/p/5342431.html
	# http://download.dre.vanderbilt.edu/
	wget ftp://download.dre.vanderbilt.edu/previous_versions/ACE-6.5.0.tar.gz
	tar zxvf ACE-6.5.0.tar.gz
	cd ACE_wrappers
	export ACE_ROOT=`pwd`
	cp ace/config-linux.h ace/config.h
	cp include/makeinclude/platform_linux.GNU include/makeinclude/platform_macros.GNU
	make
	make install INSTALL_PREFIX=/usr/local/ace
	ls -al /usr/local/ace/lib*/libACE.so
	# create link for visual studio Intellisense
	ln -s /usr/local/ace/include/ace /usr/local/include/ace
	cd $ROOTDIR
	exit 1
fi



# SSL
# https://www.cnblogs.com/fanqisoft/p/10765038.html
# https://www.bookstack.cn/read/tidb-v2.1/how-to-secure-generate-self-signed-certificates.md
cd $ROOTDIR
wget https://pkg.cfssl.org/R1.2/cfssl_linux-amd64
chmod +x cfssl_linux-amd64
wget https://pkg.cfssl.org/R1.2/cfssljson_linux-amd64
chmod +x cfssljson_linux-amd64
wget https://pkg.cfssl.org/R1.2/cfssl-certinfo_linux-amd64
chmod +x cfssl-certinfo_linux-amd64
mv cfssl_linux-amd64 /usr/local/bin/cfssl
mv cfssljson_linux-amd64 /usr/local/bin/cfssljson
mv cfssl-certinfo_linux-amd64 /usr/local/bin/cfssl-certinfo


