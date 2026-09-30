"""Exact Xbox directory roots and descendants use the same host mapping."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]


class PartitionRootTests(unittest.TestCase):
    def test_actual_rules_and_prefix_matcher(self):
        source=(ROOT/'src/kernel/kernel_path.c').read_text(encoding='utf-8')
        helper=re.search(r'static int match_prefix\(.*?\n\}',source,re.S)[0]
        rules=source[source.index('typedef struct {'):source.index('/* ======================================================================== */')]
        prelude='#include <assert.h>\n#include <ctype.h>\n#include <stdio.h>\n#include <string.h>\n'
        tail=r'''
static int find(const char *path,int *skip){
 for(int i=0;i<PATH_RULE_COUNT;i++)if((*skip=match_prefix(path,s_rules[i].prefix)))return i;
 return -1;
}
int main(void){
 unsigned tested=0;int skip;
 for(int i=0;i<PATH_RULE_COUNT;i++){
  char root[256],child[256],mixed[256],fake[256];
  strcpy(root,s_rules[i].prefix);size_t len=strlen(root);
  assert(len>1 && root[len-1]=='\\');root[len-1]=0;
  int exact=find(root,&skip);assert(exact>=0 && skip==(int)len-1);
  assert(s_rules[exact].to_save==s_rules[i].to_save);
  assert(match_prefix(s_rules[i].prefix,s_rules[i].prefix)==(int)len);
  snprintf(child,sizeof(child),"%sitem.bin",s_rules[i].prefix);
  assert(match_prefix(child,s_rules[i].prefix)==(int)len);
  strcpy(mixed,root);for(unsigned j=0;mixed[j];j++)mixed[j]=(char)toupper((unsigned char)mixed[j]);
  assert(match_prefix(mixed,s_rules[i].prefix)==(int)len-1);
  snprintf(fake,sizeof(fake),"%s0",root);assert(!match_prefix(fake,s_rules[i].prefix));
  snprintf(fake,sizeof(fake),"%sOther\\item",root);assert(!match_prefix(fake,s_rules[i].prefix));
  ++tested;
 }
 int p5=find("\\Device\\Harddisk0\\partition5",&skip);assert(p5>=0);
 assert(s_rules[p5].to_save && !strcmp(s_rules[p5].sub_win,"\\Cache\\Z"));
 assert(!strcmp(s_rules[p5].sub_posix,"/Cache/Z"));
 assert(find("\\Device\\Harddisk0\\Partition50",&skip)<0);
 assert(find("\\Device\\Harddisk0\\Partition5wrong",&skip)<0);
 assert(find("",&skip)<0);
 printf("%u directory rules: exact roots, separators, descendants, case and lookalikes passed\n",tested);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='merc-partition-root-') as directory:
            path=Path(directory);c=path/'test.c';exe=path/'test.exe'
            c.write_text(prelude+helper+rules+tail,encoding='utf-8')
            build=subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-std=c11',str(c),'-o',str(exe)],capture_output=True)
            self.assertEqual(build.returncode,0,build.stderr.decode(errors='replace'))
            run=subprocess.run([str(exe)],capture_output=True)
            self.assertEqual(run.returncode,0,run.stderr.decode(errors='replace'))
            print(run.stdout.decode().strip())
        # Partition0 is a backing file, handled before the directory loop in
        # both platform implementations, not an unreachable special case.
        bodies=re.findall(r'BOOL xbox_translate_path\(.*?\n\}',source,re.S)
        self.assertEqual(len(bodies),2)
        for body in bodies:
            self.assertLess(body.index('if (path_equals('),body.index('for (int i = 0;'))


if __name__=='__main__':unittest.main()
