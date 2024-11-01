#include <sys/types.h>
#include <regex.h>

#include "common/common.h"
#include "common/msg.h"
#include "misc/bstr.h"
#include "options/options.h"
#include "sd.h"

struct priv {
    int offset;
    regex_t *regexes;
    int num_regexes;
};

static bool rf_init(struct sd_filter *ft)
{
    if (strcmp(ft->codec, "ass") != 0)
        return false;

    if (!ft->opts->rf_enable)
        return false;

    struct priv *p = talloc_zero(ft, struct priv);
    ft->priv = p;

    for (int n = 0; ft->opts->rf_items && ft->opts->rf_items[n]; n++) {
        char *item = ft->opts->rf_items[n];

        MP_TARRAY_GROW(p, p->regexes, p->num_regexes);
        regex_t *preg = &p->regexes[p->num_regexes];

        int err = regcomp(preg, item, REG_ICASE | REG_EXTENDED | REG_NOSUB | REG_NEWLINE);
        if (err) {
            char errbuf[512];
            regerror(err, preg, errbuf, sizeof(errbuf));
            MP_ERR(ft, "Regular expression error: '%s'\n", errbuf);
            continue;
        }

        p->num_regexes += 1;
    }

    if (!p->num_regexes)
        return false;

    p->offset = sd_ass_fmt_offset(ft->event_format);
    return true;
}

static void rf_uninit(struct sd_filter *ft)
{
    struct priv *p = ft->priv;

    for (int n = 0; n < p->num_regexes; n++)
        regfree(&p->regexes[n]);
}

static struct demux_packet *rf_filter(struct sd_filter *ft,
                                      struct demux_packet *pkt)
{
    struct priv *p = ft->priv;
    char *text = bstrto0(NULL, sd_ass_pkt_text(ft, pkt, p->offset));
    bool drop = false;

    if (ft->opts->rf_plain)
        sd_ass_to_plaintext(text, strlen(text), text);

    for (int n = 0; n < p->num_regexes; n++) {
        int err = regexec(&p->regexes[n], text, 0, NULL, 0);
        if (err == 0) {
            int level = ft->opts->rf_warn ? MSGL_WARN : MSGL_V;
            MP_MSG(ft, level, "Matching regex %d => drop: '%s'\n", n, text);
            drop = true;
            break;
        } else if (err != REG_NOMATCH) {
            MP_WARN(ft, "Error on regexec() on regex %d.\n", n);
        }
    }

    talloc_free(text);
    return drop ? NULL : pkt;
}

const struct sd_filter_functions sd_filter_regex = {
    .init   = rf_init,
    .uninit = rf_uninit,
    .filter = rf_filter,
};
