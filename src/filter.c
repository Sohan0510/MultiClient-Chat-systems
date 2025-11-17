/* filter.c - naive profanity filter */
#include <stdio.h>
#include <string.h>

int main() {
    char buf[8192];
    if (!fgets(buf, sizeof(buf), stdin)) return 0;
    const char *bad[] = {"nigga","fuck","shit","bollocks","bugger","ass","asshole","anal"
,"blowjob"
,"clitoris"
,"dildo"
,"ejaculation"
,"genital"
,"masturbate"
,"penis"
,"scrotum"
,"testicle"
,"vagina"
,"wank"
,"bastard"
,"bitch"
,"bullshit"
,"cocksucker"
,"cock"
,"cunt"
,"dick"
,"douchebag"
,"fuck"
,"motherfucker"
,"niga"
,"nigga"
,"nigger"
,"piss"
,"prick"
,"pussy"
,"shit"
,"slut"
,"whore"
,"crap"
,"douche"
,"feck"
,"jerk"
,"pissed"
,"pissed off"
,"slag"
,"tits"
,"twat"
,"evilword", NULL};
    for (int i = 0; bad[i]; ++i) {
        char *p = buf;
        size_t blen = strlen(bad[i]);
        while ((p = strstr(p, bad[i]))) {
            memset(p, '*', blen);
            p += blen;
        }
    }
    printf("%s", buf);
    return 0;
}
