/*
 * This file is part of PowerDNS or dnsdist.
 * Copyright -- PowerDNS.COM B.V. and its contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * In addition, for the avoidance of any doubt, permission is granted to
 * link this program with OpenSSL and to (re)distribute the binaries
 * produced as the result of such linking.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#pragma once
#include <string>
#include <map>
#include <vector>
#include <set>
#include <stdio.h>
#include <sys/stat.h>

#include "namespaces.hh"

class BindDomainInfo 
{
public:
  void clear() 
  {
    name=ZoneName();
    filename=type="";
    primaries.clear();
    alsoNotify.clear();
    d_dev=0;
    d_ino=0;
  }
  ZoneName name;
  string viewName;
  string filename;
  vector<ComboAddress> primaries;
  set<string> alsoNotify;
  string type;
  bool hadFileDirective{false};

  dev_t d_dev{0};
  ino_t d_ino{0};

  bool operator<(const BindDomainInfo& b) const
  {
    return pair(d_dev, d_ino) < pair(b.d_dev, b.d_ino);
  }
};

extern const char *bind_directory;
extern FILE *yyin;
class BindParser
{
 public:
   BindParser()
   {
     yyin = 0;
     extern int include_stack_ptr;
     include_stack_ptr = 0;

     bind_directory = d_dir.c_str();
   }
  ~BindParser()
  {
    if(yyin) {
      fclose(yyin);
      yyin=0;
    }
  }
  void parse(const string &fname);
  void commit(BindDomainInfo DI);
  void setDirectory(const string &dir);
  const string &getDirectory();
  const vector<BindDomainInfo>& getDomains();
  void setVerbose(bool verbose);
  void addAlsoNotify(const string &host);
  set<string> & getAlsoNotify() { return this->alsoNotify; } 
private:
  string d_dir{"."};
  set<string> alsoNotify;
  vector<BindDomainInfo> d_zonedomains;
  bool d_verbose{false};
};
