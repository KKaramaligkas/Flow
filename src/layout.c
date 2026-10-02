#include <string.h>
#include "layout.h"
int browser_layout(const browser_document *doc,browser_line *lines,int maximum,float width,browser_measure_fn measure,void *ud)
{
 size_t pos=0,length=strlen(doc->text);int count=0;
 while(pos<length&&count<maximum){size_t start=pos,last_space=start;float used=0,space_width=0,height=11;int align=browser_style_at(doc,start).align;
  while(pos<length&&doc->text[pos]!='\n'){size_t n=1;while(pos+n<length&&((unsigned char)doc->text[pos+n]&0xc0)==0x80)n++;
   browser_style style=browser_style_at(doc,pos);float w=measure(ud,doc->text+pos,n,style),h=style.scale*18+2;
   if(used+w>width&&pos>start){if(last_space>start){pos=last_space;used=space_width;}break;}
   if(h>height)height=h;used+=w;pos+=n;if(doc->text[pos-n]==' '){last_space=pos;space_width=used;}
  }
  lines[count++]=(browser_line){start,pos-start,used,height,align};
  if(pos<length&&doc->text[pos]=='\n')pos++;
 }
 return count;
}
