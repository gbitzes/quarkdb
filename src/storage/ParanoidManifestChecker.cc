// ----------------------------------------------------------------------
// File: ParanoidManifestChecker.cc
// Author: Georgios Bitzes - CERN
// ----------------------------------------------------------------------

/************************************************************************
 * quarkdb - a redis-like highly available key-value store              *
 * Copyright (C) 2020 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your option) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

#include "storage/ParanoidManifestChecker.hh"
#include "utils/DirectoryIterator.hh"
#include "utils/StringUtils.hh"
#include <sys/stat.h>

namespace quarkdb {

ParanoidManifestChecker::ParanoidManifestChecker(std::string_view path)
: mPath(path) {
  mThread.reset(&ParanoidManifestChecker::main, this);
}

void ParanoidManifestChecker::main(ThreadAssistant &assistant) {
  while(!assistant.terminationRequested()) {

    Status st = checkDB(mPath);
    if(!st.ok()) {
      qdb_error("Potential MANIFEST corruption for DB at " << mPath << "(" << st.getMsg() << "). Note: This detection mechanism for MANIFEST corruption can be iffy, time to worry only if this message starts appearing every 5 minutes.");
    }

    mLastStatus.set(st);
    assistant.wait_for(std::chrono::minutes(5));
  }
}

bool operator<(struct timespec &one, struct timespec &two) {
  if(one.tv_sec == two.tv_sec) {
    return one.tv_nsec < two.tv_nsec;
  }

  return one.tv_sec < two.tv_sec;
}

std::string timespecToString(struct timespec &spec) {
  return SSTR(spec.tv_sec << "." << spec.tv_nsec);
}

Status ParanoidManifestChecker::checkDB(std::string_view path) {
  DirectoryIterator iter(path);
  struct dirent* entry = nullptr;

  struct timespec manifestMtime;
  manifestMtime.tv_sec = 0;

  struct timespec sstMtime;
  sstMtime.tv_sec = 0;

  while((entry = iter.next())) {
    struct stat statbuf;

    if(stat(SSTR(path << "/" << entry->d_name).c_str(), &statbuf) == 0) {
      if(StringUtils::startsWith(entry->d_name, "MANIFEST") && manifestMtime < statbuf.st_mtim) {
        manifestMtime = statbuf.st_mtim;
      }

      if(StringUtils::endsWith(entry->d_name, ".sst") && sstMtime < statbuf.st_mtim) {
        sstMtime = statbuf.st_mtim;
      }
    }
  }

  return compareMTimes(manifestMtime, sstMtime);
}

//------------------------------------------------------------------------------
// Get last status
//------------------------------------------------------------------------------
Status ParanoidManifestChecker::getLastStatus() const {
  return mLastStatus.get();
}

//------------------------------------------------------------------------------
// Compare mtimes, verify if sane
//------------------------------------------------------------------------------
Status ParanoidManifestChecker::compareMTimes(struct timespec manifest, struct timespec newestSst) {
  int secDiff = newestSst.tv_sec - manifest.tv_sec;
  std::string diff = SSTR(secDiff << " sec, sst:" << timespecToString(newestSst) << " vs m:" << timespecToString(manifest));

  // 1 hour should be more than enough (?)
  if(manifest.tv_sec != 0 && newestSst.tv_sec != 0 && secDiff >= 3600) {
    return Status(1, diff);
  }

  return Status(0, diff);
}

}
