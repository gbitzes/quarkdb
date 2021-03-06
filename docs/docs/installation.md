# Installation

## Packages

There are several ways to obtain QuarkDB, the easiest being to install from an RPM.
If running CentOS 7, store the following in `/etc/yum.repos.d/quarkdb.repo`:

```
[quarkdb-stable]
name=QuarkDB repository [stable]
baseurl=http://storage-ci.web.cern.ch/storage-ci/quarkdb/tag/el7/x86_64/
enabled=1
gpgcheck=False
```

Then, run `yum install quarkdb quarkdb-debuginfo`, and you're done.

## Building from source

Requirements / Dependencies:

  * Check out `utils/el7-packages.sh` for a list of build dependencies.
  * Build will fail with older versions of gcc/gcc-c++
    * On CC7, run `yum install centos-release-sc && yum install devtoolset-8 && source /opt/rh/devtoolset-8/enable`

The following will compile QuarkDB and run the tests.

```
git clone https://gitlab.cern.ch/eos/quarkdb.git && cd quarkdb
git submodule update --recursive --init

mkdir build && cd build
cmake ..
make
./test/quarkdb-tests
```

RocksDB is embedded as a submodule, but you can also compile it yourself
and specify `-DROCKSDB_ROOT_DIR` to the cmake invocation, in order to speed
things up if you do a full recompilation of QuarkDB often.
