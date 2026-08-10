#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main() {
    FILE *f = fopen("source/utils/glutil.c", "r");
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *string = malloc(fsize + 1);
    fread(string, fsize, 1, f);
    fclose(f);
    string[fsize] = 0;

    char *tgt1 = "void glColor4x_soloader(GLfixed red, GLfixed green, GLfixed blue, GLfixed alpha) {\n    glColor4f(red / 65536.0f, green / 65536.0f, blue / 65536.0f, alpha / 65536.0f);\n}";
    char *rep1 = "void glColor4x_soloader(GLfixed red, GLfixed green, GLfixed blue, GLfixed alpha) {\n    static int c_count = 0;\n    if(c_count++ < 10) l_debug(\"[GL] glColor4x r=%.2f g=%.2f b=%.2f a=%.2f\", red/65536.0f, green/65536.0f, blue/65536.0f, alpha/65536.0f);\n    glColor4f(red / 65536.0f, green / 65536.0f, blue / 65536.0f, alpha / 65536.0f);\n}";

    char *tgt2 = "void glOrthox_soloader(GLfixed left, GLfixed right, GLfixed bottom, GLfixed top, GLfixed zNear, GLfixed zFar) {\n    glOrthof(left / 65536.0f, right / 65536.0f, bottom / 65536.0f, top / 65536.0f, zNear / 65536.0f, zFar / 65536.0f);\n}";
    char *rep2 = "void glOrthox_soloader(GLfixed left, GLfixed right, GLfixed bottom, GLfixed top, GLfixed zNear, GLfixed zFar) {\n    l_debug(\"[GL] glOrthox l=%.1f r=%.1f b=%.1f t=%.1f\", left/65536.0f, right/65536.0f, bottom/65536.0f, top/65536.0f);\n    glOrthof(left / 65536.0f, right / 65536.0f, bottom / 65536.0f, top / 65536.0f, zNear / 65536.0f, zFar / 65536.0f);\n}";

    char *p = strstr(string, tgt1);
    if(p) {
        char *new_str = malloc(fsize + 1000);
        int pos = p - string;
        memcpy(new_str, string, pos);
        strcpy(new_str + pos, rep1);
        strcpy(new_str + pos + strlen(rep1), p + strlen(tgt1));
        free(string);
        string = new_str;
    }
    
    p = strstr(string, tgt2);
    if(p) {
        char *new_str = malloc(strlen(string) + 1000);
        int pos = p - string;
        memcpy(new_str, string, pos);
        strcpy(new_str + pos, rep2);
        strcpy(new_str + pos + strlen(rep2), p + strlen(tgt2));
        free(string);
        string = new_str;
    }

    f = fopen("source/utils/glutil.c", "w");
    fprintf(f, "%s", string);
    fclose(f);
    return 0;
}
