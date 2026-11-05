// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/scene_resource_history.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"%d: %s\n",__LINE__,#x);std::exit(1);}}while(false)
using namespace GekkoAOT::GX;
int main(){
  SceneResourceHistory history;
  CHECK(history.Read(1,0)==0);
  const auto a=history.NextPass(),b=history.NextPass();CHECK(a==1 && b==2);
  history.Write(1,1,a);CHECK(history.Read(1,1)==a);
  CHECK(history.Read(1,0)==0 && history.Read(1,2)==0);
  history.Write(1,2,b);CHECK(history.Read(1,1)==0 && history.Read(1,2)==b);
  // A new texture lifetime receives a different ID even if a GPU address is
  // reused, and its first revision must not resolve to the old allocation.
  CHECK(history.Read(2,2)==0);history.Write(2,1,b);CHECK(history.Read(2,1)==b);
  history.Write(0,1,a);history.Write(3,1,0);CHECK(history.Read(0,1)==0 && history.Read(3,1)==0);
  for(unsigned i=4;i<SceneResourceHistory::Capacity+8;++i)history.Write(i,1,history.NextPass());
  CHECK(history.Read(1,2)==0);CHECK(history.Read(SceneResourceHistory::Capacity+7,1)>0);
  history.Reset();CHECK(history.Read(2,1)==0 && history.NextPass()==1);
  std::puts("Scene resource history: exact revisions, independent lifetimes, missing producers, eviction and reset passed");
}
