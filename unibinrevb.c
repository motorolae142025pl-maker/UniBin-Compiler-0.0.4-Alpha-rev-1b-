#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define LINE_MAX_LEN 8192
#define MAX_LINES 4096
#define MAX_FUNCS 128
#define MAX_BUTTONS 64

static void usage(const char *argv0) {
    fprintf(stderr,
        "UniBin Compiler 0.0.4 (Linux prototype)\n"
        "Usage: %s input.ub [-o output] [--emit-c output.c] [--lib-dir directory]\n"
        "Imports named in $import are bundled into the generated C source and executable.\n",
        argv0);
}
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}
static int starts(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }
static void ident(const char *in, char *out, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < cap; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c == '_') out[j++] = (char)c;
        else if (c == '-' && j + 1 < cap) out[j++] = '_';
    }
    out[j] = '\0';
    if (!j || isdigit((unsigned char)out[0])) {
        if (j + 2 < cap) { memmove(out + 1, out, j + 1); out[0] = '_'; }
    }
}
static int decode_quoted(const char **src, char *out, size_t cap) {
    const char *p = *src;
    while (isspace((unsigned char)*p)) p++;
    if (*p != '"') return 0;
    p++;
    size_t n = 0;
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            if (!*p) return 0;
            c = (unsigned char)*p++;
            if (c == 'n') c = '\n'; else if (c == 't') c = '\t'; else if (c == 'r') c = '\r';
            else if (c != '\\' && c != '"') return 0;
        }
        if (n + 1 >= cap) return 0;
        out[n++] = (char)c;
    }
    if (*p != '"') return 0;
    out[n] = '\0'; *src = p + 1; return 1;
}
static void emit_c_string(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        switch (*p) {
            case '\\': fputs("\\\\", out); break;
            case '"': fputs("\\\"", out); break;
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
            default: if (*p < 32 || *p >= 127) fprintf(out, "\\x%02x", *p); else fputc(*p, out);
        }
    }
    fputc('"', out);
}
static int parse_int(const char *s, int *value, const char **endout) {
    while (isspace((unsigned char)*s)) s++;
    errno = 0; char *end = NULL; long v = strtol(s, &end, 10);
    if (errno || end == s || v < -32768 || v > 32767) return 0;
    *value = (int)v; if (endout) *endout = end; return 1;
}
static int parse_timeout(const char *s, int *value) {
    char buf[64]; const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '"') { if (!decode_quoted(&p, buf, sizeof buf)) return 0; p = buf; }
    const char *end; if (!parse_int(p, value, &end)) return 0;
    while (isspace((unsigned char)*end)) end++;
    if (*end == ')') end++;
    while (isspace((unsigned char)*end)) end++;
    if (*end == ';') end++;
    while (isspace((unsigned char)*end)) end++;
    return *end == '\0' && *value >= 0 && *value <= 86400;
}

typedef struct { char name[128]; int line_start, line_end; } Func;
typedef struct { char alias[128]; char label[512]; int id; } Button;
static Func funcs[MAX_FUNCS]; static int nfuncs;
static Button buttons[MAX_BUTTONS]; static int nbuttons;
static char *lines[MAX_LINES]; static int nlines;
#define MAX_IMPORTS 128
static char imports[MAX_IMPORTS][128];
static int nimports;
static const char *library_dir = "lib";
static const char *source_output = NULL;
static int main_start = -1, main_end = -1;
static int find_func(const char *name) {
    char wanted[128]; ident(name, wanted, sizeof wanted);
    for (int i = 0; i < nfuncs; i++) if (!strcmp(funcs[i].name, wanted)) return i;
    return -1;
}
/* Parse the standalone command form: goto function_name; */
static int parse_goto_statement(const char *s, char *name, size_t cap) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (strncmp(p, "goto", 4) != 0 || (p[4] && !isspace((unsigned char)p[4]))) return 0;
    p += 4;
    while (isspace((unsigned char)*p)) p++;
    size_t n = 0;
    while (isalnum((unsigned char)*p) || *p == '_' || *p == '-') {
        if (n + 1 >= cap) return -1;
        name[n++] = *p++;
    }
    if (!n) return -1;
    name[n] = '\0';
    while (isspace((unsigned char)*p)) p++;
    if (*p++ != ';') return -1;
    while (isspace((unsigned char)*p)) p++;
    return *p == '\0' ? 1 : -1;
}
static int find_button(const char *alias) {
    for (int i = 0; i < nbuttons; i++) if (!strcmp(buttons[i].alias, alias)) return i;
    return -1;
}
static int parse_button_line(const char *s, int lineno, const char *input) {
    const char *p = strchr(s, '('); if (!p) return 0; p++;
    while (*p && *p != ')') {
        while (isspace((unsigned char)*p) || *p == ',') p++;
        if (*p == ')' || !*p) break;
        char alias[128]; size_t a = 0;
        while ((isalnum((unsigned char)*p) || *p == '_' || *p == '-') && a + 1 < sizeof alias) alias[a++] = *p++;
        alias[a] = '\0'; while (isspace((unsigned char)*p)) p++;
        char label[512];
        if (!a || !decode_quoted(&p, label, sizeof label)) {
            fprintf(stderr, "%s:%d: ?2 error: expected button syntax alias \"label\"; id\n", input, lineno); return -1;
        }
        while (isspace((unsigned char)*p)) p++;
        if (*p != ';') { fprintf(stderr, "%s:%d: ?2 error: expected ';' before button ID\n", input, lineno); return -1; }
        p++; int id; const char *end;
        if (!parse_int(p, &id, &end) || id < 0) { fprintf(stderr, "%s:%d: ?2 error: button ID must be a non-negative integer\n", input, lineno); return -1; }
        p = end;
        if (nbuttons >= MAX_BUTTONS) { fprintf(stderr, "%s:%d: ?2 error: too many buttons (max %d)\n", input, lineno, MAX_BUTTONS); return -1; }
        if (find_button(alias) >= 0) { fprintf(stderr, "%s:%d: ?2 error: duplicate button alias '%s'\n", input, lineno, alias); return -1; }
        Button *b = &buttons[nbuttons++]; snprintf(b->alias, sizeof b->alias, "%s", alias); snprintf(b->label, sizeof b->label, "%s", label); b->id = id;
        while (isspace((unsigned char)*p)) p++;
        if (*p == ',') p++; else if (*p != ')' && *p != '\0') { fprintf(stderr, "%s:%d: ?2 error: expected ',' between buttons\n", input, lineno); return -1; }
    }
    return 1;
}
static int is_builtin_import(const char *name) {
    return !strcmp(name, "base") || !strcmp(name, "gui-disp") ||
           !strcmp(name, "gui-comp") || !strcmp(name, "player");
}

static int valid_import_name(const char *name) {
    if (!*name) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (!isalnum(*p) && *p != '_' && *p != '-') return 0;
    return 1;
}

static int add_import(const char *name, const char *input, int line) {
    if (!valid_import_name(name)) {
        fprintf(stderr, "%s:%d: ?2 error: invalid library name '%s'\n", input, line, name);
        return 0;
    }
    for (int i = 0; i < nimports; ++i)
        if (!strcmp(imports[i], name)) return 1; /* duplicate imports are harmless */
    if (nimports >= MAX_IMPORTS) {
        fprintf(stderr, "%s:%d: ?2 error: too many imports (max %d)\n", input, line, MAX_IMPORTS);
        return 0;
    }
    snprintf(imports[nimports++], sizeof imports[0], "%s", name);
    return 1;
}

static int has_import(const char *name) {
    for (int i = 0; i < nimports; ++i)
        if (!strcmp(imports[i], name)) return 1;
    return 0;
}

static int embed_one_library(FILE *out, const char *input, const char *name) {
    char path[PATH_MAX];
    int n = snprintf(path, sizeof path, "%s/%s.c", library_dir, name);
    if (n < 0 || (size_t)n >= sizeof path) {
        fprintf(stderr, "UniBin: library path too long for '%s'\n", name);
        return 0;
    }
    FILE *lib = fopen(path, "r");
    if (!lib) {
        fprintf(stderr, "%s: ?2 error: cannot load library '%s' (looked for %s)\n", input, name, path);
        return 0;
    }
    fprintf(out, "\n/* ---- bundled UniBin library: %s ---- */\n", name);
    char buf[4096]; size_t got;
    while ((got = fread(buf, 1, sizeof buf, lib)) > 0) {
        if (fwrite(buf, 1, got, out) != got) {
            fprintf(stderr, "UniBin: failed writing bundled library '%s'\n", name);
            fclose(lib); return 0;
        }
    }
    if (ferror(lib)) {
        fprintf(stderr, "UniBin: failed reading library '%s'\n", path);
        fclose(lib); return 0;
    }
    fclose(lib);
    fputs("\n/* ---- end bundled library ---- */\n", out);
    return 1;
}

/* Built-ins are bundled in dependency order; custom libraries follow import order. */
static int embed_imported_libraries(FILE *out, const char *input) {
    static const char *builtins[] = {"base", "gui-disp", "gui-comp", "player"};
    for (size_t b = 0; b < sizeof builtins / sizeof builtins[0]; ++b)
        if (has_import(builtins[b]) && !embed_one_library(out, input, builtins[b])) return 0;
    for (int i = 0; i < nimports; ++i) {
        if (is_builtin_import(imports[i])) continue;
        if (!embed_one_library(out, input, imports[i])) return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 2; }
    const char *input = argv[1], *output = "a.out";
    for (int i=2;i<argc;i++) {
        if (!strcmp(argv[i], "-o") && i+1<argc) output=argv[++i];
        else if (!strcmp(argv[i], "--emit-c") && i+1<argc) source_output=argv[++i];
        else if (!strcmp(argv[i], "--lib-dir") && i+1<argc) library_dir=argv[++i];
        else { usage(argv[0]); return 2; }
    }
    FILE *in=fopen(input,"r"); if(!in){perror(input);return 1;}
    char buf[LINE_MAX_LEN]; int lineno=0;
    while(fgets(buf,sizeof buf,in)){ if(nlines>=MAX_LINES){fprintf(stderr,"%s: ?2 error: source has too many lines\n",input);fclose(in);return 1;} lines[nlines++]=strdup(buf); lineno++; if(!strchr(buf,'\n')&&!feof(in)){fprintf(stderr,"%s:%d: ?2 error: line too long\n",input,lineno);fclose(in);return 1;} }
    fclose(in);
    /* Collect imports before parsing executable statements. */
    for (int i = 0; i < nlines; ++i) {
        char *line = trim(lines[i]);
        if (!starts(line, "$import")) continue;
        if (line[7] != '\0' && !isspace((unsigned char)line[7])) continue;
        char *name = trim(line + 7);
        size_t len = strlen(name);
        if (len && name[len - 1] == ';') name[--len] = '\0';
        name = trim(name);
        if (!*name || strpbrk(name, " \t\r\n")) {
            fprintf(stderr, "%s:%d: ?2 error: malformed $import; expected $import library-name\n", input, i + 1);
            return 1;
        }
        if (!add_import(name, input, i + 1)) return 1;
    }
    /* Components use the display/window runtime. Add that dependency automatically. */
    if (has_import("gui-comp") && !has_import("gui-disp")) {
        if (!add_import("gui-disp", input, 0)) return 1;
    }
    /* Find main and function ranges; function bodies end at int exit; or next function. */
    int seen_main=0, in_main=0, in_meta=0, active_func=-1;
    for(int i=0;i<nlines;i++){
        char *s=trim(lines[i]); if(!*s||starts(s,"//")||starts(s,"#"))continue;
        if(starts(s,"$import "))continue;
        if(starts(s,"public void meta")){in_meta=1;continue;} if(in_meta){if(strchr(s,'}'))in_meta=0;continue;}
        if(starts(s,"public void main")){if(seen_main){fprintf(stderr,"%s:%d: ?2 error: duplicate main declaration\n",input,i+1);return 1;}seen_main=1;in_main=1;main_start=i;continue;}
        if(!in_main)continue;
        if(starts(s,"def func")){
            if(active_func>=0)funcs[active_func].line_end=i-1;
            if(nfuncs>=MAX_FUNCS){fprintf(stderr,"%s:%d: ?2 error: too many functions\n",input,i+1);return 1;}
            const char *p=strchr(s,'('); if(!p){fprintf(stderr,"%s:%d: ?2 error: expected def func (name);\n",input,i+1);return 1;} p++;
            char raw[128];size_t j=0;while(*p&&*p!=')'&&j+1<sizeof raw){if(!isspace((unsigned char)*p))raw[j++]=*p;p++;}raw[j]='\0';
            Func *f=&funcs[nfuncs];ident(raw,f->name,sizeof f->name);f->line_start=i+1;f->line_end=nlines-1;active_func=nfuncs++;continue;
        }
        if(active_func>=0 && starts(s,"int exit;")){funcs[active_func].line_end=i;active_func=-1;continue;}
        if(!strcmp(s,"}")||starts(s,"};")){if(active_func>=0){funcs[active_func].line_end=i-1;active_func=-1;}main_end=i;in_main=0;break;}
    }
    if(!seen_main||main_start<0||main_end<0){fprintf(stderr,"%s: ?2 error: missing or unclosed 'public void main; { ... }'\n",input);return 1;}
    /* Parse button declarations in main, skipping complete function bodies. */
    for(int i=main_start+1;i<main_end;i++){
        char *s=trim(lines[i]);
        if(starts(s,"def func")){
            int fi=-1; const char *np=strchr(s,'('); char raw[128]={0};
            if(np){np++;size_t nj=0;while(*np&&*np!=')'&&nj+1<sizeof raw){if(!isspace((unsigned char)*np))raw[nj++]=*np;np++;}raw[nj]='\0';fi=find_func(raw);}
            if(fi>=0)i=funcs[fi].line_end;
            continue;
        }
        if(starts(s,"init.gui-comp::butt(")){int r=parse_button_line(s,i+1,input);if(r<0)return 1;}
    }
    /* Ensure all handler names exist. */
    for(int i=main_start+1;i<main_end;i++){
        char *s=trim(lines[i]); if(starts(s,"if ")&&strstr(s,".press")&&strstr(s,"goto func ")){
            char *p=strstr(s,"goto func ")+10; char name[128];size_t j=0;while(*p&&(isalnum((unsigned char)*p)||*p=='_'||*p=='-')&&j+1<sizeof name)name[j++]=*p++;name[j]='\0';
            if(find_func(name)<0){fprintf(stderr,"%s:%d: ?2 error: handler function '%s' is not defined\n",input,i+1,name);return 1;}
        }
    }
    char tmp[]="/tmp/unibin-XXXXXX.c"; int fd=mkstemps(tmp,2);if(fd<0){perror("mkstemps");return 1;}FILE*out=fdopen(fd,"w");if(!out){perror("fdopen");close(fd);unlink(tmp);return 1;}
    fputs("#define _POSIX_C_SOURCE 200809L\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <unistd.h>\n\n", out);
    if (!embed_imported_libraries(out, input)) { fclose(out); unlink(tmp); return 1; }
    /* Function prototypes and bodies are emitted before main. */
    for(int i=0;i<nfuncs;i++)fprintf(out,"static void ub_func_%s(void);\n",funcs[i].name);
    for(int fi=0;fi<nfuncs;fi++){
        fprintf(out,"static void ub_func_%s(void) {\n",funcs[fi].name);
        fprintf(out,"ub_func_%s_start:\n",funcs[fi].name);
        for(int i=funcs[fi].line_start;i<=funcs[fi].line_end&&i<nlines;i++){
            char *s=trim(lines[i]); if(!*s||starts(s,"//")||starts(s,"#")||starts(s,"def func"))continue;
            if(starts(s,"goto")){char target[128];int gr=parse_goto_statement(s,target,sizeof target);if(gr!=1){fprintf(stderr,"%s:%d: ?2 error: expected standalone 'goto function_name;'\n",input,i+1);return 1;}int target_index=find_func(target);if(target_index<0){fprintf(stderr,"%s:%d: ?2 error: function '%s' is not defined\n",input,i+1,target);return 1;}if(target_index==fi)fprintf(out,"goto ub_func_%s_start;\n",funcs[fi].name);else {char target_ident[128];ident(target,target_ident,sizeof target_ident);fprintf(out,"ub_func_%s();\n",target_ident);}}
            else if(starts(s,"console.log(")){const char*p=strchr(s,'(')+1;char text[LINE_MAX_LEN];if(!decode_quoted(&p,text,sizeof text)){fprintf(stderr,"%s:%d: ?2 error: malformed console.log\n",input,i+1);return 1;}fputs("fputs(",out);emit_c_string(out,text);fputs(",stdout);fputc('\\n',stdout);\n",out);}
            else if(starts(s,"console.timeout(")){int sec;if(!parse_timeout(strchr(s,'(')+1,&sec)){fprintf(stderr,"%s:%d: ?2 error: timeout must be 0-86400 seconds\n",input,i+1);return 1;}fprintf(out,"ub_base_timeout(%d);\n",sec);}
            else if(starts(s,"audio.play(")){const char*p=strchr(s,'(')+1;char path[LINE_MAX_LEN];if(!decode_quoted(&p,path,sizeof path)){fprintf(stderr,"%s:%d: ?2 error: expected audio.play(\"file\")\n",input,i+1);return 1;}fputs("ub_audio_play(",out);emit_c_string(out,path);fputs(");\n",out);}
            else if(starts(s,"audio.stop("))fputs("ub_audio_stop();\n",out);
            else if(starts(s,"audio.pause("))fputs("ub_audio_pause();\n",out);
            else if(starts(s,"audio.resume("))fputs("ub_audio_resume();\n",out);
            else if(starts(s,"init.gui-comp::msgbox(")){const char*p=strchr(s,'(')+1;char msg[LINE_MAX_LEN];if(!decode_quoted(&p,msg,sizeof msg)){fprintf(stderr,"%s:%d: ?2 error: expected msgbox(\"text\")\n",input,i+1);return 1;}fputs("ub_gui_msgbox(",out);emit_c_string(out,msg);fputs(");\n",out);}
            else if(starts(s,"int exit;")){
                if(has_import("gui-disp")||has_import("gui-comp"))fputs("ub_gui_exit(); return;\n",out);
                else fputs("exit(0);\n",out);
            }
            else if(starts(s,"return ")){
                int v;const char*p=s+7,*e;
                if(!parse_int(p,&v,&e)||v<0||v>255){fprintf(stderr,"%s:%d: ?2 error: return value must be 0-255\n",input,i+1);return 1;}
                if(has_import("gui-disp")||has_import("gui-comp"))fputs("ub_gui_exit(); return;\n",out);
                else fprintf(out,"exit(%d);\n",v);
            }
            else if(starts(s,"if ")&&strstr(s,".press")&&strstr(s,"goto func "))continue;
            else if(starts(s,"if ")&&strstr(s,".press")&&strstr(s,"goto int exit"))continue;
            else if(starts(s,"init.gui-disp::window("))continue;
            else if(starts(s,"init.gui-comp::label("))continue;
            else if(starts(s,"init.gui-comp::butt("))continue;
            else if(starts(s,"log.handler"))continue;
            else if(starts(s,"audio.")) {fprintf(stderr,"%s:%d: ?2 error: unsupported audio operation\n",input,i+1);return 1;}
            else {fprintf(stderr,"%s:%d: ?2 error: unsupported statement in function: %s\n",input,i+1,s);return 1;}
        }
        fputs("}\n",out);
    }
    fputs("int main(int argc,char **argv){(void)argc;(void)argv;\n",out);
    int has_gui=0, has_explicit_return=0;
    for(int i=main_start+1;i<main_end;i++){
        char *s=trim(lines[i]); if(!*s||starts(s,"//")||starts(s,"#")||starts(s,"$import "))continue;
        if(starts(s,"def func")){ int fi=-1; const char *np=strchr(s,'('); char raw[128]={0}; if(np){np++;size_t nj=0;while(*np&&*np!=')'&&nj+1<sizeof raw){if(!isspace((unsigned char)*np))raw[nj++]=*np;np++;}raw[nj]='\0';fi=find_func(raw);} if(fi>=0){fprintf(out,"ub_func_%s();\n",funcs[fi].name);i=funcs[fi].line_end;} continue; }
        if(starts(s,"public void meta"))continue;
        if(starts(s,"goto")){char target[128];int gr=parse_goto_statement(s,target,sizeof target);if(gr!=1){fprintf(stderr,"%s:%d: ?2 error: expected standalone 'goto function_name;'\n",input,i+1);return 1;}if(find_func(target)<0){fprintf(stderr,"%s:%d: ?2 error: function '%s' is not defined\n",input,i+1,target);return 1;}char target_ident[128];ident(target,target_ident,sizeof target_ident);fprintf(out,"ub_func_%s();\n",target_ident);}
        else if(starts(s,"console.log(")){const char*p=strchr(s,'(')+1;char text[LINE_MAX_LEN];if(!decode_quoted(&p,text,sizeof text)){fprintf(stderr,"%s:%d: ?2 error: expected console.log(\"text\")\n",input,i+1);return 1;}fputs("fputs(",out);emit_c_string(out,text);fputs(",stdout);fputc('\\n',stdout);\n",out);}
        else if(starts(s,"console.timeout(")){int sec;if(!parse_timeout(strchr(s,'(')+1,&sec)){fprintf(stderr,"%s:%d: ?2 error: timeout must be 0-86400 seconds\n",input,i+1);return 1;}fprintf(out,"ub_base_timeout(%d);\n",sec);}
        else if(starts(s,"audio.play(")){const char*p=strchr(s,'(')+1;char path[LINE_MAX_LEN];if(!decode_quoted(&p,path,sizeof path)){fprintf(stderr,"%s:%d: ?2 error: expected audio.play(\"file\")\n",input,i+1);return 1;}fputs("ub_audio_play(",out);emit_c_string(out,path);fputs(");\n",out);}
        else if(starts(s,"audio.stop("))fputs("ub_audio_stop();\n",out);
        else if(starts(s,"audio.pause("))fputs("ub_audio_pause();\n",out);
        else if(starts(s,"audio.resume("))fputs("ub_audio_resume();\n",out);
        else if(starts(s,"init.gui-disp::window(")){const char*p=strchr(s,'(')+1;char title[512];if(!decode_quoted(&p,title,sizeof title)){fprintf(stderr,"%s:%d: ?2 error: expected window(\"title\"; width; height)\n",input,i+1);return 1;}while(isspace((unsigned char)*p))p++;if(*p!=';'){fprintf(stderr,"%s:%d: ?2 error: expected width after window title\n",input,i+1);return 1;}p++;int w,h;const char*e;if(!parse_int(p,&w,&e)){fprintf(stderr,"%s:%d: ?2 error: invalid window width\n",input,i+1);return 1;}p=e;while(isspace((unsigned char)*p))p++;if(*p!=';'){fprintf(stderr,"%s:%d: ?2 error: expected height\n",input,i+1);return 1;}p++;if(!parse_int(p,&h,NULL)||w<100||h<80){fprintf(stderr,"%s:%d: ?2 error: window size must be at least 100x80\n",input,i+1);return 1;}fputs("ub_gui_window(",out);emit_c_string(out,title);fprintf(out,",%d,%d);\n",w,h);has_gui=1;}
        else if(starts(s,"init.gui-comp::label(")){const char*p=strchr(s,'(')+1;char text[512];if(!decode_quoted(&p,text,sizeof text)){fprintf(stderr,"%s:%d: ?2 error: expected label(\"text\"; x; y)\n",input,i+1);return 1;}while(isspace((unsigned char)*p))p++;if(*p!=';'){fprintf(stderr,"%s:%d: ?2 error: expected label x coordinate\n",input,i+1);return 1;}p++;int x,y;const char*e;if(!parse_int(p,&x,&e)){fprintf(stderr,"%s:%d: ?2 error: invalid label x coordinate\n",input,i+1);return 1;}p=e;while(isspace((unsigned char)*p))p++;if(*p!=';'){fprintf(stderr,"%s:%d: ?2 error: expected label y coordinate\n",input,i+1);return 1;}p++;if(!parse_int(p,&y,NULL)){fprintf(stderr,"%s:%d: ?2 error: invalid label y coordinate\n",input,i+1);return 1;}fputs("ub_gui_label(",out);emit_c_string(out,text);fprintf(out,",%d,%d);\n",x,y);has_gui=1;}
        else if(starts(s,"init.gui-comp::butt(")){for(int b=0;b<nbuttons;b++){fputs("ub_gui_button(",out);emit_c_string(out,buttons[b].label);fprintf(out,",%d);\n",buttons[b].id);}has_gui=1;}
        else if(starts(s,"if ")&&strstr(s,".press")&&strstr(s,"goto func ")){char alias[128],name[128];const char*p=s+3;size_t j=0;while((isalnum((unsigned char)*p)||*p=='_'||*p=='-')&&j+1<sizeof alias)alias[j++]=*p++;alias[j]='\0';const char*q=strstr(s,"goto func ");if(!q){continue;}q+=10;j=0;while((isalnum((unsigned char)*q)||*q=='_'||*q=='-')&&j+1<sizeof name)name[j++]=*q++;name[j]='\0';int bi=find_button(alias);if(bi<0){fprintf(stderr,"%s:%d: ?2 error: unknown button alias '%s'\n",input,i+1,alias);return 1;}char fname[128];ident(name,fname,sizeof fname);fprintf(out,"ub_gui_bind(%d,ub_func_%s);\n",buttons[bi].id,fname);has_gui=1;}
        else if(starts(s,"if ")&&strstr(s,".press")&&strstr(s,"goto int exit")){char alias[128];const char*p=s+3;size_t j=0;while((isalnum((unsigned char)*p)||*p=='_'||*p=='-')&&j+1<sizeof alias)alias[j++]=*p++;alias[j]='\0';int bi=find_button(alias);if(bi<0){fprintf(stderr,"%s:%d: ?2 error: unknown button alias '%s'\n",input,i+1,alias);return 1;}fprintf(out,"ub_gui_bind(%d,ub_gui_exit);\n",buttons[bi].id);has_gui=1;}
        else if(starts(s,"init.gui-comp::msgbox(")){const char*p=strchr(s,'(')+1;char msg[512];if(!decode_quoted(&p,msg,sizeof msg)){fprintf(stderr,"%s:%d: ?2 error: expected msgbox(\"text\")\n",input,i+1);return 1;}fputs("ub_gui_msgbox(",out);emit_c_string(out,msg);fputs(");\n",out);}
        else if(starts(s,"int exit;")){if(has_gui)fputs("ub_gui_exit();\n",out);else fputs("return 0;\n",out);has_explicit_return=1;}
        else if(starts(s,"return ")){int v;const char*p=s+7,*e;if(!parse_int(p,&v,&e)||v<0||v>255){fprintf(stderr,"%s:%d: ?2 error: return value must be 0-255\n",input,i+1);return 1;}if(has_gui)fputs("ub_gui_run(); ",out);if(has_import("player"))fputs("ub_audio_stop(); ",out);fputs("return ",out);fprintf(out,"%d;\n",v);has_explicit_return=1;}
        else if(starts(s,"log.handler")) {fprintf(stderr,"%s:%d: ?2 error: log.handler routing is not implemented in 0.0.2 yet\n",input,i+1);return 1;}
        else if(starts(s,"audio.")){fprintf(stderr,"%s:%d: ?2 error: unsupported audio operation\n",input,i+1);return 1;}
        else {fprintf(stderr,"%s:%d: ?2 error: unsupported statement: %s\n",input,i+1,s);fprintf(stderr,"Supported: console.log/timeout, audio.play/stop/pause/resume, gui window/label/button/msgbox, button handlers, return, int exit.\n");return 1;}
    }
    if(has_gui)fputs("ub_gui_run();\n",out);
    if(!has_explicit_return){if(has_import("player"))fputs("ub_audio_stop(); ",out);fputs("return 0;\n",out);}
    fputs("}\n",out);
    if(fclose(out)!=0){perror("close generated C");unlink(tmp);return 1;}
    if (source_output) {
        FILE *src = fopen(tmp, "rb");
        FILE *dst = fopen(source_output, "wb");
        if (!src || !dst) {
            perror(source_output);
            if (src) fclose(src);
            if (dst) fclose(dst);
            unlink(tmp);
            return 1;
        }
        char copybuf[8192]; size_t count;
        while ((count = fread(copybuf, 1, sizeof copybuf, src)) > 0)
            if (fwrite(copybuf, 1, count, dst) != count) { perror(source_output); fclose(src); fclose(dst); unlink(tmp); return 1; }
        if (ferror(src)) { perror("read generated C"); fclose(src); fclose(dst); unlink(tmp); return 1; }
        fclose(src);
        if (fclose(dst) != 0) { perror(source_output); unlink(tmp); return 1; }
    }
    pid_t pid=fork();if(pid<0){perror("fork");unlink(tmp);return 1;}
    if(pid==0){
        if (has_import("gui-disp"))
            execlp("cc","cc","-std=c11","-O2","-Wall","-Wextra","-Wno-unused-function","-o",output,tmp,"-lX11",(char*)NULL);
        else
            execlp("cc","cc","-std=c11","-O2","-Wall","-Wextra","-Wno-unused-function","-o",output,tmp,(char*)NULL);
        perror("cc");_exit(127);
    }
    int status;if(waitpid(pid,&status,0)<0){perror("waitpid");unlink(tmp);return 1;}unlink(tmp);
    if(!WIFEXITED(status)||WEXITSTATUS(status)!=0)return 1;
    printf("UniBin: built native Linux executable '%s'\n",output);
    if (source_output) printf("UniBin: wrote generated C source '%s'\n", source_output);
    if (nimports) printf("UniBin: processed %d unique import(s)\n", nimports);
    for(int i=0;i<nlines;i++)free(lines[i]);
    return 0;
}
