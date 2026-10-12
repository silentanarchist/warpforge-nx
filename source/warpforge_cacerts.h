#ifndef WARPFORGE_CACERTS_H
#define WARPFORGE_CACERTS_H
typedef struct { const char *name; const unsigned char *der; unsigned len; } WfCaCert;
extern const WfCaCert wf_cacerts[];
extern const unsigned wf_cacerts_n;
#endif
