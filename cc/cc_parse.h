#ifndef _CC_PARSE_H_
#define _CC_PARSE_H_

void next();
int extern_search(char* name);
void typecheck(int op, int tl, int tr);
void expr(int lev);
void stmt(int ctx);


#endif
