#!/usr/bin/env bash
set -e

##------------------------------------------------------------------------------
## Bootstrap packages - needed to run 'builddep' on quarkdb for the next
## step.
##------------------------------------------------------------------------------

dnf install -y gcc-c++ cmake3 make rpm-build which git yum-plugin-priorities yum-utils libtsan

##------------------------------------------------------------------------------
## Extract quarkdb build dependencies from its specfile.
##------------------------------------------------------------------------------

mkdir build
pushd build
cmake3 .. -DPACKAGEONLY=1
make srpm
yum-builddep -y SRPMS/*
popd

##------------------------------------------------------------------------------
## Install rocksdb
##------------------------------------------------------------------------------

ci/install-rocksdb.sh
