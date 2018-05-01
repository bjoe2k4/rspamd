/*-
 * Copyright 2016 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "config.h"
#include "util.h"
#include "rspamd.h"
#include "message.h"
#include "html.h"
#include "html_tags.h"
#include "html_colors.h"
#include "url.h"
#include <unicode/uversion.h>
#include <unicode/ucnv.h>
#if U_ICU_VERSION_MAJOR_NUM >= 46
#include <unicode/uidna.h>
#endif

static sig_atomic_t tags_sorted = 0;
static sig_atomic_t entities_sorted = 0;
static const guint max_tags = 8192; /* Ignore tags if this maximum is reached */

struct html_tag_def {
	const gchar *name;
	gint16 id;
	guint16 len;
	guint flags;
};

#define msg_debug_html(...)  rspamd_conditional_debug_fast (NULL, NULL, \
        rspamd_html_log_id, "html", pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)

INIT_LOG_MODULE(html)

#define TAG_DEF(id, name, flags) {(name), (id), (sizeof(name) - 1), (flags)}

static struct html_tag_def tag_defs[] = {
	/* W3C defined elements */
	TAG_DEF(Tag_A, "a", 0),
	TAG_DEF(Tag_ABBR, "abbr", (CM_INLINE)),
	TAG_DEF(Tag_ACRONYM, "acronym", (CM_INLINE)),
	TAG_DEF(Tag_ADDRESS, "address", (CM_BLOCK)),
	TAG_DEF(Tag_APPLET, "applet", (CM_OBJECT | CM_IMG | CM_INLINE | CM_PARAM)),
	TAG_DEF(Tag_AREA, "area", (CM_BLOCK | CM_EMPTY)),
	TAG_DEF(Tag_B, "b", (CM_INLINE|FL_BLOCK)),
	TAG_DEF(Tag_BASE, "base", (CM_HEAD | CM_EMPTY)),
	TAG_DEF(Tag_BASEFONT, "basefont", (CM_INLINE | CM_EMPTY)),
	TAG_DEF(Tag_BDO, "bdo", (CM_INLINE)),
	TAG_DEF(Tag_BIG, "big", (CM_INLINE)),
	TAG_DEF(Tag_BLOCKQUOTE, "blockquote", (CM_BLOCK)),
	TAG_DEF(Tag_BODY, "body", (CM_HTML | CM_OPT | CM_OMITST | CM_UNIQUE | FL_BLOCK)),
	TAG_DEF(Tag_BR, "br", (CM_INLINE | CM_EMPTY)),
	TAG_DEF(Tag_BUTTON, "button", (CM_INLINE|FL_BLOCK)),
	TAG_DEF(Tag_CAPTION, "caption", (CM_TABLE)),
	TAG_DEF(Tag_CENTER, "center", (CM_BLOCK)),
	TAG_DEF(Tag_CITE, "cite", (CM_INLINE)),
	TAG_DEF(Tag_CODE, "code", (CM_INLINE)),
	TAG_DEF(Tag_COL, "col", (CM_TABLE | CM_EMPTY)),
	TAG_DEF(Tag_COLGROUP, "colgroup", (CM_TABLE | CM_OPT)),
	TAG_DEF(Tag_DD, "dd", (CM_DEFLIST | CM_OPT | CM_NO_INDENT)),
	TAG_DEF(Tag_DEL, "del", (CM_INLINE | CM_BLOCK | CM_MIXED)),
	TAG_DEF(Tag_DFN, "dfn", (CM_INLINE)),
	TAG_DEF(Tag_DIR, "dir", (CM_BLOCK | CM_OBSOLETE)),
	TAG_DEF(Tag_DIV, "div", (CM_BLOCK|FL_BLOCK)),
	TAG_DEF(Tag_DL, "dl", (CM_BLOCK|FL_BLOCK)),
	TAG_DEF(Tag_DT, "dt", (CM_DEFLIST | CM_OPT | CM_NO_INDENT)),
	TAG_DEF(Tag_EM, "em", (CM_INLINE)),
	TAG_DEF(Tag_FIELDSET, "fieldset", (CM_BLOCK)),
	TAG_DEF(Tag_FONT, "font", (FL_BLOCK)),
	TAG_DEF(Tag_FORM, "form", (CM_BLOCK)),
	TAG_DEF(Tag_FRAME, "frame", (CM_FRAMES | CM_EMPTY)),
	TAG_DEF(Tag_FRAMESET, "frameset", (CM_HTML | CM_FRAMES)),
	TAG_DEF(Tag_H1, "h1", (CM_BLOCK | CM_HEADING)),
	TAG_DEF(Tag_H2, "h2", (CM_BLOCK | CM_HEADING)),
	TAG_DEF(Tag_H3, "h3", (CM_BLOCK | CM_HEADING)),
	TAG_DEF(Tag_H4, "h4", (CM_BLOCK | CM_HEADING)),
	TAG_DEF(Tag_H5, "h5", (CM_BLOCK | CM_HEADING)),
	TAG_DEF(Tag_H6, "h6", (CM_BLOCK | CM_HEADING)),
	TAG_DEF(Tag_HEAD, "head", (CM_HTML | CM_OPT | CM_OMITST | CM_UNIQUE)),
	TAG_DEF(Tag_HR, "hr", (CM_BLOCK | CM_EMPTY)),
	TAG_DEF(Tag_HTML, "html", (CM_HTML | CM_OPT | CM_OMITST | CM_UNIQUE)),
	TAG_DEF(Tag_I, "i", (CM_INLINE)),
	TAG_DEF(Tag_IFRAME, "iframe", (0)),
	TAG_DEF(Tag_IMG, "img", (CM_INLINE | CM_IMG | CM_EMPTY)),
	TAG_DEF(Tag_INPUT, "input", (CM_INLINE | CM_IMG | CM_EMPTY)),
	TAG_DEF(Tag_INS, "ins", (CM_INLINE | CM_BLOCK | CM_MIXED)),
	TAG_DEF(Tag_ISINDEX, "isindex", (CM_BLOCK | CM_EMPTY)),
	TAG_DEF(Tag_KBD, "kbd", (CM_INLINE)),
	TAG_DEF(Tag_LABEL, "label", (CM_INLINE)),
	TAG_DEF(Tag_LEGEND, "legend", (CM_INLINE)),
	TAG_DEF(Tag_LI, "li", (CM_LIST | CM_OPT | CM_NO_INDENT | FL_BLOCK)),
	TAG_DEF(Tag_LINK, "link", (CM_HEAD | CM_EMPTY)),
	TAG_DEF(Tag_LISTING, "listing", (CM_BLOCK | CM_OBSOLETE)),
	TAG_DEF(Tag_MAP, "map", (CM_INLINE)),
	TAG_DEF(Tag_MENU, "menu", (CM_BLOCK | CM_OBSOLETE)),
	TAG_DEF(Tag_META, "meta", (CM_HEAD | CM_INLINE | CM_EMPTY)),
	TAG_DEF(Tag_NOFRAMES, "noframes", (CM_BLOCK | CM_FRAMES)),
	TAG_DEF(Tag_NOSCRIPT, "noscript", (CM_BLOCK | CM_INLINE | CM_MIXED)),
	TAG_DEF(Tag_OBJECT, "object", (CM_OBJECT | CM_HEAD | CM_IMG | CM_INLINE | CM_PARAM)),
	TAG_DEF(Tag_OL, "ol", (CM_BLOCK | FL_BLOCK)),
	TAG_DEF(Tag_OPTGROUP, "optgroup", (CM_FIELD | CM_OPT)),
	TAG_DEF(Tag_OPTION, "option", (CM_FIELD | CM_OPT)),
	TAG_DEF(Tag_P, "p", (CM_BLOCK | CM_OPT | FL_BLOCK)),
	TAG_DEF(Tag_PARAM, "param", (CM_INLINE | CM_EMPTY)),
	TAG_DEF(Tag_PLAINTEXT, "plaintext", (CM_BLOCK | CM_OBSOLETE)),
	TAG_DEF(Tag_PRE, "pre", (CM_BLOCK)),
	TAG_DEF(Tag_Q, "q", (CM_INLINE)),
	TAG_DEF(Tag_RB, "rb", (CM_INLINE)),
	TAG_DEF(Tag_RBC, "rbc", (CM_INLINE)),
	TAG_DEF(Tag_RP, "rp", (CM_INLINE)),
	TAG_DEF(Tag_RT, "rt", (CM_INLINE)),
	TAG_DEF(Tag_RTC, "rtc", (CM_INLINE)),
	TAG_DEF(Tag_RUBY, "ruby", (CM_INLINE)),
	TAG_DEF(Tag_S, "s", (CM_INLINE)),
	TAG_DEF(Tag_SAMP, "samp", (CM_INLINE)),
	TAG_DEF(Tag_SCRIPT, "script", (CM_HEAD | CM_MIXED)),
	TAG_DEF(Tag_SELECT, "select", (CM_INLINE | CM_FIELD)),
	TAG_DEF(Tag_SMALL, "small", (CM_INLINE)),
	TAG_DEF(Tag_SPAN, "span", (CM_BLOCK|FL_BLOCK)),
	TAG_DEF(Tag_STRIKE, "strike", (CM_INLINE)),
	TAG_DEF(Tag_STRONG, "strong", (CM_INLINE)),
	TAG_DEF(Tag_STYLE, "style", (CM_HEAD)),
	TAG_DEF(Tag_SUB, "sub", (CM_INLINE)),
	TAG_DEF(Tag_SUP, "sup", (CM_INLINE)),
	TAG_DEF(Tag_TABLE, "table", (CM_BLOCK | FL_BLOCK)),
	TAG_DEF(Tag_TBODY, "tbody", (CM_TABLE | CM_ROWGRP | CM_OPT| FL_BLOCK)),
	TAG_DEF(Tag_TD, "td", (CM_ROW | CM_OPT | CM_NO_INDENT | FL_BLOCK)),
	TAG_DEF(Tag_TEXTAREA, "textarea", (CM_INLINE | CM_FIELD)),
	TAG_DEF(Tag_TFOOT, "tfoot", (CM_TABLE | CM_ROWGRP | CM_OPT)),
	TAG_DEF(Tag_TH, "th", (CM_ROW | CM_OPT | CM_NO_INDENT | FL_BLOCK)),
	TAG_DEF(Tag_THEAD, "thead", (CM_TABLE | CM_ROWGRP | CM_OPT)),
	TAG_DEF(Tag_TITLE, "title", (CM_HEAD | CM_UNIQUE)),
	TAG_DEF(Tag_TR, "tr", (CM_TABLE | CM_OPT| FL_BLOCK)),
	TAG_DEF(Tag_TT, "tt", (CM_INLINE)),
	TAG_DEF(Tag_U, "u", (CM_INLINE)),
	TAG_DEF(Tag_UL, "ul", (CM_BLOCK|FL_BLOCK)),
	TAG_DEF(Tag_VAR, "var", (CM_INLINE)),
	TAG_DEF(Tag_XMP, "xmp", (CM_BLOCK | CM_OBSOLETE)),
	TAG_DEF(Tag_NEXTID, "nextid", (CM_HEAD | CM_EMPTY)),

	/* proprietary elements */
	TAG_DEF(Tag_ALIGN, "align", (CM_BLOCK)),
	TAG_DEF(Tag_BGSOUND, "bgsound", (CM_HEAD | CM_EMPTY)),
	TAG_DEF(Tag_BLINK, "blink", (CM_INLINE)),
	TAG_DEF(Tag_COMMENT, "comment", (CM_INLINE)),
	TAG_DEF(Tag_EMBED, "embed", (CM_INLINE | CM_IMG | CM_EMPTY)),
	TAG_DEF(Tag_ILAYER, "ilayer", (CM_INLINE)),
	TAG_DEF(Tag_KEYGEN, "keygen", (CM_INLINE | CM_EMPTY)),
	TAG_DEF(Tag_LAYER, "layer", (CM_BLOCK)),
	TAG_DEF(Tag_MARQUEE, "marquee", (CM_INLINE | CM_OPT)),
	TAG_DEF(Tag_MULTICOL, "multicol", (CM_BLOCK)),
	TAG_DEF(Tag_NOBR, "nobr", (CM_INLINE)),
	TAG_DEF(Tag_NOEMBED, "noembed", (CM_INLINE)),
	TAG_DEF(Tag_NOLAYER, "nolayer", (CM_BLOCK | CM_INLINE | CM_MIXED)),
	TAG_DEF(Tag_NOSAVE, "nosave", (CM_BLOCK)),
	TAG_DEF(Tag_SERVER, "server", (CM_HEAD | CM_MIXED | CM_BLOCK | CM_INLINE)),
	TAG_DEF(Tag_SERVLET, "servlet", (CM_OBJECT | CM_IMG | CM_INLINE | CM_PARAM)),
	TAG_DEF(Tag_SPACER, "spacer", (CM_INLINE | CM_EMPTY)),
	TAG_DEF(Tag_WBR, "wbr", (CM_INLINE | CM_EMPTY)),
};

struct _entity;
typedef struct _entity entity;

struct _entity {
	gchar *name;
	uint code;
	gchar *replacement;
};


static entity entities_defs[] = {
	/*
	** Markup pre-defined character entities
	*/
	{"quot", 34, "\""},
	{"amp", 38, "&"},
	{"apos", 39, "'"},
	{"lt", 60, "<"},
	{"gt", 62, ">"},

	/*
	** Latin-1 character entities
	*/
	{"nbsp", 160, " "},
	{"iexcl", 161, "!"},
	{"cent", 162, "cent"},
	{"pound", 163, "pound"},
	{"curren", 164, "current"},
	{"yen", 165, "yen"},
	{"brvbar", 166, NULL},
	{"sect", 167, NULL},
	{"uml", 168, "uml"},
	{"copy", 169, "c"},
	{"ordf", 170, NULL},
	{"laquo", 171, "\""},
	{"not", 172, "!"},
	{"shy", 173, NULL},
	{"reg", 174, "r"},
	{"macr", 175, NULL},
	{"deg", 176, "deg"},
	{"plusmn", 177, "+-"},
	{"sup2", 178, "2"},
	{"sup3", 179, "3"},
	{"acute", 180, NULL},
	{"micro", 181, NULL},
	{"para", 182, NULL},
	{"middot", 183, "."},
	{"cedil", 184, NULL},
	{"sup1", 185, "1"},
	{"ordm", 186, NULL},
	{"raquo", 187, "\""},
	{"frac14", 188, "1/4"},
	{"frac12", 189, "1/2"},
	{"frac34", 190, "3/4"},
	{"iquest", 191, "i"},
	{"Agrave", 192, "a"},
	{"Aacute", 193, "a"},
	{"Acirc", 194, "a"},
	{"Atilde", 195, "a"},
	{"Auml", 196, "a"},
	{"Aring", 197, "a"},
	{"AElig", 198, "a"},
	{"Ccedil", 199, "c"},
	{"Egrave", 200, "e"},
	{"Eacute", 201, "e"},
	{"Ecirc", 202, "e"},
	{"Euml", 203, "e"},
	{"Igrave", 204, "i"},
	{"Iacute", 205, "i"},
	{"Icirc", 206, "i"},
	{"Iuml", 207, "i"},
	{"ETH", 208, "e"},
	{"Ntilde", 209, "n"},
	{"Ograve", 210, "o"},
	{"Oacute", 211, "o"},
	{"Ocirc", 212, "o"},
	{"Otilde", 213, "o"},
	{"Ouml", 214, "o"},
	{"times", 215, "t"},
	{"Oslash", 216, "o"},
	{"Ugrave", 217, "u"},
	{"Uacute", 218, "u"},
	{"Ucirc", 219, "u"},
	{"Uuml", 220, "u"},
	{"Yacute", 221, "y"},
	{"THORN", 222, "t"},
	{"szlig", 223, "s"},
	{"agrave", 224, "a"},
	{"aacute", 225, "a"},
	{"acirc", 226, "a"},
	{"atilde", 227, "a"},
	{"auml", 228, "a"},
	{"aring", 229, "a"},
	{"aelig", 230, "a"},
	{"ccedil", 231, "c"},
	{"egrave", 232, "e"},
	{"eacute", 233, "e"},
	{"ecirc", 234, "e"},
	{"euml", 235, "e"},
	{"igrave", 236, "e"},
	{"iacute", 237, "e"},
	{"icirc", 238, "e"},
	{"iuml", 239, "e"},
	{"eth", 240, "e"},
	{"ntilde", 241, "n"},
	{"ograve", 242, "o"},
	{"oacute", 243, "o"},
	{"ocirc", 244, "o"},
	{"otilde", 245, "o"},
	{"ouml", 246, "o"},
	{"divide", 247, "/"},
	{"oslash", 248, "/"},
	{"ugrave", 249, "u"},
	{"uacute", 250, "u"},
	{"ucirc", 251, "u"},
	{"uuml", 252, "u"},
	{"yacute", 253, "y"},
	{"thorn", 254, "t"},
	{"yuml", 255, "y"},

	/*
	** Extended Entities defined in HTML 4: Symbols
	*/
	{"fnof", 402, "f"},
	{"Alpha", 913, "alpha"},
	{"Beta", 914, "beta"},
	{"Gamma", 915, "gamma"},
	{"Delta", 916, "delta"},
	{"Epsilon", 917, "epsilon"},
	{"Zeta", 918, "zeta"},
	{"Eta", 919, "eta"},
	{"Theta", 920, "theta"},
	{"Iota", 921, "iota"},
	{"Kappa", 922, "kappa"},
	{"Lambda", 923, "lambda"},
	{"Mu", 924, "mu"},
	{"Nu", 925, "nu"},
	{"Xi", 926, "xi"},
	{"Omicron", 927, "omicron"},
	{"Pi", 928, "pi"},
	{"Rho", 929, "rho"},
	{"Sigma", 931, "sigma"},
	{"Tau", 932, "tau"},
	{"Upsilon", 933, "upsilon"},
	{"Phi", 934, "phi"},
	{"Chi", 935, "chi"},
	{"Psi", 936, "psi"},
	{"Omega", 937, "omega"},
	{"alpha", 945, "alpha"},
	{"beta", 946, "beta"},
	{"gamma", 947, "gamma"},
	{"delta", 948, "delta"},
	{"epsilon", 949, "epsilon"},
	{"zeta", 950, "zeta"},
	{"eta", 951, "eta"},
	{"theta", 952, "theta"},
	{"iota", 953, "iota"},
	{"kappa", 954, "kappa"},
	{"lambda", 955, "lambda"},
	{"mu", 956, "mu"},
	{"nu", 957, "nu"},
	{"xi", 958, "xi"},
	{"omicron", 959, "omicron"},
	{"pi", 960, "pi"},
	{"rho", 961, "rho"},
	{"sigmaf", 962, "sigmaf"},
	{"sigma", 963, "sigma"},
	{"tau", 964, "tau"},
	{"upsilon", 965, "upsilon"},
	{"phi", 966, "phi"},
	{"chi", 967, "chi"},
	{"psi", 968, "psi"},
	{"omega", 969, "omega"},
	{"thetasym", 977, "thetasym"},
	{"upsih", 978, "upsih"},
	{"piv", 982, "piv"},
	{"bull", 8226, "bull"},
	{"hellip", 8230, "..."},
	{"prime", 8242, "'"},
	{"Prime", 8243, "'"},
	{"oline", 8254, "-"},
	{"frasl", 8260, NULL},
	{"weierp", 8472, NULL},
	{"image", 8465, NULL},
	{"real", 8476, NULL},
	{"trade", 8482, NULL},
	{"alefsym", 8501, "a"},
	{"larr", 8592, NULL},
	{"uarr", 8593, NULL},
	{"rarr", 8594, NULL},
	{"darr", 8595, NULL},
	{"harr", 8596, NULL},
	{"crarr", 8629, NULL},
	{"lArr", 8656, NULL},
	{"uArr", 8657, NULL},
	{"rArr", 8658, NULL},
	{"dArr", 8659, NULL},
	{"hArr", 8660, NULL},
	{"forall", 8704, NULL},
	{"part", 8706, NULL},
	{"exist", 8707, NULL},
	{"empty", 8709, NULL},
	{"nabla", 8711, NULL},
	{"isin", 8712, NULL},
	{"notin", 8713, NULL},
	{"ni", 8715, NULL},
	{"prod", 8719, NULL},
	{"sum", 8721, "E"},
	{"minus", 8722, "-"},
	{"lowast", 8727, NULL},
	{"radic", 8730, NULL},
	{"prop", 8733, NULL},
	{"infin", 8734, NULL},
	{"ang", 8736, "'"},
	{"and", 8743, "&"},
	{"or", 8744, "|"},
	{"cap", 8745, NULL},
	{"cup", 8746, NULL},
	{"gint", 8747, NULL},
	{"there4", 8756, NULL},
	{"sim", 8764, NULL},
	{"cong", 8773, NULL},
	{"asymp", 8776, NULL},
	{"ne", 8800, "!="},
	{"equiv", 8801, "=="},
	{"le", 8804, "<="},
	{"ge", 8805, ">="},
	{"sub", 8834, NULL},
	{"sup", 8835, NULL},
	{"nsub", 8836, NULL},
	{"sube", 8838, NULL},
	{"supe", 8839, NULL},
	{"oplus", 8853, NULL},
	{"otimes", 8855, NULL},
	{"perp", 8869, NULL},
	{"sdot", 8901, NULL},
	{"lceil", 8968, NULL},
	{"rceil", 8969, NULL},
	{"lfloor", 8970, NULL},
	{"rfloor", 8971, NULL},
	{"lang", 9001, NULL},
	{"rang", 9002, NULL},
	{"loz", 9674, NULL},
	{"spades", 9824, NULL},
	{"clubs", 9827, NULL},
	{"hearts", 9829, NULL},
	{"diams", 9830, NULL},

	/*
	** Extended Entities defined in HTML 4: Special (less Markup at top)
	*/
	{"OElig", 338, NULL},
	{"oelig", 339, NULL},
	{"Scaron", 352, NULL},
	{"scaron", 353, NULL},
	{"Yuml", 376, NULL},
	{"circ", 710, NULL},
	{"tilde", 732, NULL},
	{"ensp", 8194, NULL},
	{"emsp", 8195, NULL},
	{"thinsp", 8201, NULL},
	{"zwnj", 8204, NULL},
	{"zwj", 8205, NULL},
	{"lrm", 8206, NULL},
	{"rlm", 8207, NULL},
	{"ndash", 8211, "-"},
	{"mdash", 8212, "-"},
	{"lsquo", 8216, "'"},
	{"rsquo", 8217, "'"},
	{"sbquo", 8218, "\""},
	{"ldquo", 8220, "\""},
	{"rdquo", 8221, "\""},
	{"bdquo", 8222, "\""},
	{"dagger", 8224, "T"},
	{"Dagger", 8225, "T"},
	{"permil", 8240, NULL},
	{"lsaquo", 8249, "\""},
	{"rsaquo", 8250, "\""},
	{"euro", 8364, "E"},
};

static GHashTable *html_colors_hash = NULL;

static entity entities_defs_num[ (G_N_ELEMENTS (entities_defs)) ];
static struct html_tag_def tag_defs_num[ (G_N_ELEMENTS (tag_defs)) ];

static gint
tag_cmp (const void *m1, const void *m2)
{
	const struct html_tag_def *p1 = m1;
	const struct html_tag_def *p2 = m2;

	if (p1->len == p2->len) {
		return rspamd_lc_cmp (p1->name, p2->name, p1->len);
	}

	return p1->len - p2->len;
}

static gint
tag_cmp_id (const void *m1, const void *m2)
{
	const struct html_tag_def *p1 = m1;
	const struct html_tag_def *p2 = m2;

	return p1->id - p2->id;
}

static gint
tag_find_id (const void *skey, const void *elt)
{
	const struct html_tag *tag = skey;
	const struct html_tag_def *d = elt;

	return tag->id - d->id;
}

static gint
tag_find (const void *skey, const void *elt)
{
	const struct html_tag *tag = skey;
	const struct html_tag_def *d = elt;

	if (d->len == tag->name.len) {
		return rspamd_lc_cmp (tag->name.start, d->name, tag->name.len);
	}

	return tag->name.len - d->len;
}

static gint
entity_cmp (const void *m1, const void *m2)
{
	const entity *p1 = m1;
	const entity *p2 = m2;

	return g_ascii_strcasecmp (p1->name, p2->name);
}

static gint
entity_cmp_num (const void *m1, const void *m2)
{
	const entity *p1 = m1;
	const entity *p2 = m2;

	return p1->code - p2->code;
}

static void
rspamd_html_library_init (void)
{
	if (!tags_sorted) {
		qsort (tag_defs, G_N_ELEMENTS (
				tag_defs), sizeof (struct html_tag_def), tag_cmp);
		memcpy (tag_defs_num, tag_defs, sizeof (tag_defs));
		qsort (tag_defs_num, G_N_ELEMENTS (tag_defs_num),
				sizeof (struct html_tag_def), tag_cmp_id);
		tags_sorted = 1;
	}

	if (!entities_sorted) {
		qsort (entities_defs, G_N_ELEMENTS (
				entities_defs), sizeof (entity), entity_cmp);
		memcpy (entities_defs_num, entities_defs, sizeof (entities_defs));
		qsort (entities_defs_num, G_N_ELEMENTS (
				entities_defs), sizeof (entity), entity_cmp_num);
		entities_sorted = 1;
	}

	if (html_colors_hash == NULL) {
		guint i;

		html_colors_hash = g_hash_table_new_full (rspamd_ftok_icase_hash,
				rspamd_ftok_icase_equal, g_free, g_free);

		for (i = 0; i < G_N_ELEMENTS (html_colornames); i ++) {
			struct html_color *color;
			rspamd_ftok_t *key;

			color = g_malloc0 (sizeof (*color));
			color->d.comp.alpha = 255;
			color->d.comp.r = html_colornames[i].rgb.r;
			color->d.comp.g = html_colornames[i].rgb.g;
			color->d.comp.b = html_colornames[i].rgb.b;
			color->valid = TRUE;
			key = g_malloc0 (sizeof (*key));
			key->begin = html_colornames[i].name;
			key->len = strlen (html_colornames[i].name);

			g_hash_table_insert (html_colors_hash, key, color);
		}
	}
}

static gboolean
rspamd_html_check_balance (GNode * node, GNode ** cur_level)
{
	struct html_tag *arg = node->data, *tmp;
	GNode *cur;

	if (arg->flags & FL_CLOSING) {
		/* First of all check whether this tag is closing tag for parent node */
		cur = node->parent;
		while (cur && cur->data) {
			tmp = cur->data;
			if (tmp->id == arg->id &&
				(tmp->flags & FL_CLOSED) == 0) {
				tmp->flags |= FL_CLOSED;
				/* Destroy current node as we find corresponding parent node */
				g_node_destroy (node);
				/* Change level */
				*cur_level = cur->parent;
				return TRUE;
			}
			cur = cur->parent;
		}
	}
	else {
		return TRUE;
	}

	return FALSE;
}

gint
rspamd_html_tag_by_name (const gchar *name)
{
	struct html_tag tag;
	struct html_tag_def *found;

	tag.name.start = name;
	tag.name.len = strlen (name);

	found = bsearch (&tag, tag_defs, G_N_ELEMENTS (tag_defs),
			sizeof (tag_defs[0]), tag_find);

	if (found) {
		return found->id;
	}

	return -1;
}

gboolean
rspamd_html_tag_seen (struct html_content *hc, const gchar *tagname)
{
	gint id;

	g_assert (hc != NULL);
	g_assert (hc->tags_seen != NULL);

	id = rspamd_html_tag_by_name (tagname);

	if (id != -1) {
		return isset (hc->tags_seen, id);
	}

	return FALSE;
}

const gchar*
rspamd_html_tag_by_id (gint id)
{
	struct html_tag tag;
	struct html_tag_def *found;

	tag.id = id;
	/* Should work as IDs monotonically increase */
	found = bsearch (&tag, tag_defs_num, G_N_ELEMENTS (tag_defs_num),
				sizeof (tag_defs_num[0]), tag_find_id);

	if (found) {
		return found->name;
	}

	return NULL;
}

/* Decode HTML entitles in text */
guint
rspamd_html_decode_entitles_inplace (gchar *s, guint len)
{
	guint l, rep_len;
	gchar *t = s, *h = s, *e = s, *end_ptr;
	gint state = 0, val, base;
	entity *found, key;

	if (len == 0) {
		l = strlen (s);
	}
	else {
		l = len;
	}

	while (h - s < (gint)l) {
		switch (state) {
		/* Out of entitle */
		case 0:
			if (*h == '&') {
				state = 1;
				e = h;
				h++;
				continue;
			}
			else {
				*t = *h;
				h++;
				t++;
			}
			break;
		case 1:
			if (*h == ';' && h > e) {
				/* Determine base */
				/* First find in entities table */

				key.name = e + 1;
				*h = '\0';
				if (*(e + 1) != '#' &&
					(found =
					bsearch (&key, entities_defs, G_N_ELEMENTS (entities_defs),
							sizeof (entity), entity_cmp)) != NULL) {
					if (found->replacement) {
						rep_len = strlen (found->replacement);
						memcpy (t, found->replacement, rep_len);
						t += rep_len;
					}
					else {
						memmove (t, e, h - e);
						t += h - e;
					}
				}
				else if (e + 2 < h) {
					if (*(e + 2) == 'x' || *(e + 2) == 'X') {
						base = 16;
					}
					else if (*(e + 2) == 'o' || *(e + 2) == 'O') {
						base = 8;
					}
					else {
						base = 10;
					}
					if (base == 10) {
						val = strtoul ((e + 2), &end_ptr, base);
					}
					else {
						val = strtoul ((e + 3), &end_ptr, base);
					}
					if (end_ptr != NULL && *end_ptr != '\0') {
						/* Skip undecoded */
						memmove (t, e, h - e);
						t += h - e;
					}
					else {
						/* Search for a replacement */
						key.code = val;
						found =
							bsearch (&key, entities_defs_num, G_N_ELEMENTS (
									entities_defs), sizeof (entity),
								entity_cmp_num);
						if (found) {
							if (found->replacement) {
								rep_len = strlen (found->replacement);
								memcpy (t, found->replacement, rep_len);
								t += rep_len;
							}
						}
						else {
							/* Unicode point */
							if (g_unichar_isgraph (val)) {
								t += g_unichar_to_utf8 (val, t);
							}
							else {
								/* Remove unknown entities */
							}
						}
					}
				}

				*h = ';';
				state = 0;
			}
			h++;

			break;
		}
	}

	return (t - s);
}

static gboolean
rspamd_url_is_subdomain (rspamd_ftok_t *t1, rspamd_ftok_t *t2)
{
	const gchar *p1, *p2;

	p1 = t1->begin + t1->len - 1;
	p2 = t2->begin + t2->len - 1;

	/* Skip trailing dots */
	while (p1 > t1->begin) {
		if (*p1 != '.') {
			break;
		}

		p1 --;
	}

	while (p2 > t2->begin) {
		if (*p2 != '.') {
			break;
		}

		p2 --;
	}

	while (p1 > t1->begin && p2 > t2->begin) {
		if (*p1 != *p2) {
			break;
		}

		p1 --;
		p2 --;
	}

	if (p2 == t2->begin) {
		/* p2 can be subdomain of p1 if *p1 is '.' */
		if (p1 != t1->begin && *(p1 - 1) == '.') {
			return TRUE;
		}
	}
	else if (p1 == t1->begin) {
		if (p2 != t2->begin && *(p2 - 1) == '.') {
			return TRUE;
		}
	}

	return FALSE;
}

static void
rspamd_html_url_is_phished (rspamd_mempool_t *pool,
	struct rspamd_url *href_url,
	const guchar *url_text,
	gsize len,
	gboolean *url_found,
	struct rspamd_url **ptext_url)
{
	struct rspamd_url *text_url;
	rspamd_ftok_t phished_tld, disp_tok, href_tok;
	gint rc;
	goffset url_pos;
	gchar *url_str = NULL, *idn_hbuf;
	const guchar *end = url_text + len, *p;
#if U_ICU_VERSION_MAJOR_NUM >= 46
	static UIDNA *udn;
	UErrorCode uc_err = U_ZERO_ERROR;
	UIDNAInfo uinfo = UIDNA_INFO_INITIALIZER;
#endif

	*url_found = FALSE;
#if U_ICU_VERSION_MAJOR_NUM >= 46
	if (udn == NULL) {
		udn = uidna_openUTS46 (UIDNA_DEFAULT, &uc_err);

		if (uc_err != U_ZERO_ERROR) {
			msg_err_pool ("cannot init idna converter: %s", u_errorName (uc_err));
		}
	}
#endif

	while (url_text < end && g_ascii_isspace (*url_text)) {
		url_text ++;
	}

	if (end > url_text + 4 &&
			rspamd_url_find (pool, url_text, end - url_text, &url_str, FALSE,
					&url_pos, NULL) &&
			url_str != NULL) {
		if (url_pos > 0) {
			/*
			 * We have some url at some offset, so we need to check what is
			 * at the start of the text
			 */
			p = url_text;

			while (p < url_text + url_pos) {
				if (!g_ascii_isspace (*p)) {
					*url_found = FALSE;
					return;
				}

				p++;
			}
		}
		text_url = rspamd_mempool_alloc0 (pool, sizeof (struct rspamd_url));
		rc = rspamd_url_parse (text_url, url_str, strlen (url_str), pool);

		if (rc == URI_ERRNO_OK) {
			disp_tok.len = text_url->hostlen;
			disp_tok.begin = text_url->host;
#if U_ICU_VERSION_MAJOR_NUM >= 46
			if (rspamd_substring_search_caseless (text_url->host,
					text_url->hostlen, "xn--", 4) != -1) {
				idn_hbuf = rspamd_mempool_alloc (pool, text_url->hostlen * 2 + 1);
				/* We need to convert it to the normal value first */
				disp_tok.len = uidna_nameToUnicodeUTF8 (udn,
						text_url->host, text_url->hostlen,
						idn_hbuf, text_url->hostlen * 2 + 1, &uinfo, &uc_err);

				if (uc_err != U_ZERO_ERROR) {
					msg_err_pool ("cannot convert to IDN: %s",
							u_errorName (uc_err));
					disp_tok.len = text_url->hostlen;
				}
				else {
					disp_tok.begin = idn_hbuf;
				}
			}
#endif
			href_tok.len = href_url->hostlen;
			href_tok.begin = href_url->host;
#if U_ICU_VERSION_MAJOR_NUM >= 46
			if (rspamd_substring_search_caseless (href_url->host,
					href_url->hostlen, "xn--", 4) != -1) {
				idn_hbuf = rspamd_mempool_alloc (pool, href_url->hostlen * 2 + 1);
				/* We need to convert it to the normal value first */
				href_tok.len = uidna_nameToUnicodeUTF8 (udn,
						href_url->host, href_url->hostlen,
						idn_hbuf, href_url->hostlen * 2 + 1, &uinfo, &uc_err);

				if (uc_err != U_ZERO_ERROR) {
					msg_err_pool ("cannot convert to IDN: %s",
							u_errorName (uc_err));
					href_tok.len = href_url->hostlen;
				}
				else {
					href_tok.begin = idn_hbuf;
				}
			}
#endif
			if (rspamd_ftok_casecmp (&disp_tok, &href_tok) != 0) {

				/* Apply the same logic for TLD */
				disp_tok.len = text_url->tldlen;
				disp_tok.begin = text_url->tld;
#if U_ICU_VERSION_MAJOR_NUM >= 46
				if (rspamd_substring_search_caseless (text_url->tld,
						text_url->tldlen, "xn--", 4) != -1) {
					idn_hbuf = rspamd_mempool_alloc (pool, text_url->tldlen * 2 + 1);
					/* We need to convert it to the normal value first */
					disp_tok.len = uidna_nameToUnicodeUTF8 (udn,
							text_url->tld, text_url->tldlen,
							idn_hbuf, text_url->tldlen * 2 + 1, &uinfo, &uc_err);

					if (uc_err != U_ZERO_ERROR) {
						msg_err_pool ("cannot convert to IDN: %s",
								u_errorName (uc_err));
						disp_tok.len = text_url->tldlen;
					}
					else {
						disp_tok.begin = idn_hbuf;
					}
				}
#endif
				href_tok.len = href_url->tldlen;
				href_tok.begin = href_url->tld;
#if U_ICU_VERSION_MAJOR_NUM >= 46
				if (rspamd_substring_search_caseless (href_url->tld,
						href_url->tldlen, "xn--", 4) != -1) {
					idn_hbuf = rspamd_mempool_alloc (pool, href_url->tldlen * 2 + 1);
					/* We need to convert it to the normal value first */
					href_tok.len = uidna_nameToUnicodeUTF8 (udn,
							href_url->tld, href_url->tldlen,
							idn_hbuf, href_url->tldlen * 2 + 1, &uinfo, &uc_err);

					if (uc_err != U_ZERO_ERROR) {
						msg_err_pool ("cannot convert to IDN: %s",
								u_errorName (uc_err));
						href_tok.len = href_url->tldlen;
					}
					else {
						href_tok.begin = idn_hbuf;
					}
				}
#endif
				if (rspamd_ftok_casecmp (&disp_tok, &href_tok) != 0) {
					/* Check if one url is a subdomain for another */

					if (!rspamd_url_is_subdomain (&disp_tok, &href_tok)) {
						href_url->flags |= RSPAMD_URL_FLAG_PHISHED;
						href_url->phished_url = text_url;
						phished_tld.begin = href_tok.begin;
						phished_tld.len = href_tok.len;
						rspamd_url_add_tag (text_url, "phishing",
								rspamd_mempool_ftokdup (pool, &phished_tld),
								pool);
						text_url->flags |= RSPAMD_URL_FLAG_HTML_DISPLAYED;
					}
				}
			}

			*ptext_url = text_url;
			*url_found = TRUE;
		}
		else {
			msg_info_pool ("extract of url '%s' failed: %s",
					url_str,
					rspamd_url_strerror (rc));
		}
	}

}

static gboolean
rspamd_html_process_tag (rspamd_mempool_t *pool, struct html_content *hc,
		struct html_tag *tag, GNode **cur_level, gboolean *balanced)
{
	GNode *nnode;
	struct html_tag *parent;

	if (hc->html_tags == NULL) {
		nnode = g_node_new (NULL);
		*cur_level = nnode;
		hc->html_tags = nnode;
		rspamd_mempool_add_destructor (pool,
				(rspamd_mempool_destruct_t) g_node_destroy,
				nnode);
	}

	if (hc->total_tags > max_tags) {
		hc->flags |= RSPAMD_HTML_FLAG_TOO_MANY_TAGS;
	}

	if (tag->id == -1) {
		/* Ignore unknown tags */
		hc->total_tags ++;
		return FALSE;
	}

	tag->parent = *cur_level;

	if (!(tag->flags & CM_INLINE)) {
		/* Block tag */
		if (tag->flags & (FL_CLOSING|FL_CLOSED)) {
			if (!*cur_level) {
				msg_debug_html ("bad parent node");
				return FALSE;
			}

			if (hc->total_tags < max_tags) {
				nnode = g_node_new (tag);
				g_node_append (*cur_level, nnode);

				if (!rspamd_html_check_balance (nnode, cur_level)) {
					msg_debug_html (
							"mark part as unbalanced as it has not pairable closing tags");
					hc->flags |= RSPAMD_HTML_FLAG_UNBALANCED;
					*balanced = FALSE;
				} else {
					*balanced = TRUE;
				}

				hc->total_tags ++;
			}
		}
		else {
			parent = (*cur_level)->data;

			if (parent) {
				if ((parent->flags & FL_IGNORE)) {
					tag->flags |= FL_IGNORE;
				}

				if (!(tag->flags & FL_CLOSED) &&
						!(parent->flags & FL_BLOCK)) {
					/* We likely have some bad nesting */
					if (parent->id == tag->id) {
						/* Something like <a>bla<a>foo... */
						hc->flags |= RSPAMD_HTML_FLAG_UNBALANCED;
						*balanced = FALSE;
						tag->parent = parent->parent;

						if (hc->total_tags < max_tags) {
							nnode = g_node_new (tag);
							g_node_append (parent->parent, nnode);
							*cur_level = nnode;
							hc->total_tags ++;
						}

						return TRUE;
					}
				}

				parent->content_length += tag->content_length;
			}

			if (hc->total_tags < max_tags) {
				nnode = g_node_new (tag);
				g_node_append (*cur_level, nnode);

				if ((tag->flags & FL_CLOSED) == 0) {
					*cur_level = nnode;
				}

				hc->total_tags ++;
			}

			if (tag->flags & (CM_HEAD|CM_UNKNOWN|FL_IGNORE)) {
				tag->flags |= FL_IGNORE;

				return FALSE;
			}

		}
	}
	else {
		/* Inline tag */
		parent = (*cur_level)->data;

		if (parent && (parent->flags & (CM_HEAD|CM_UNKNOWN|FL_IGNORE))) {
			tag->flags |= FL_IGNORE;

			return FALSE;
		}
	}

	return TRUE;
}

#define NEW_COMPONENT(comp_type) do {							\
	comp = rspamd_mempool_alloc (pool, sizeof (*comp));			\
	comp->type = (comp_type);									\
	comp->start = NULL;											\
	comp->len = 0;												\
	g_queue_push_tail (tag->params, comp);						\
	ret = TRUE;													\
} while(0)

static gboolean
rspamd_html_parse_tag_component (rspamd_mempool_t *pool,
		const guchar *begin, const guchar *end,
		struct html_tag *tag)
{
	struct html_tag_component *comp;
	gint len;
	gboolean ret = FALSE;
	gchar *p;

	g_assert (end >= begin);
	p = rspamd_mempool_alloc (pool, end - begin);
	memcpy (p, begin, end - begin);
	len = rspamd_html_decode_entitles_inplace (p, end - begin);

	if (len == 3) {
		if (g_ascii_strncasecmp (p, "src", len) == 0) {
			NEW_COMPONENT (RSPAMD_HTML_COMPONENT_HREF);
		}
	}
	else if (len == 4) {
		if (g_ascii_strncasecmp (p, "href", len) == 0) {
			NEW_COMPONENT (RSPAMD_HTML_COMPONENT_HREF);
		}
	}
	else if (tag->id == Tag_IMG) {
		/* Check width and height if presented */
		if (len == 5 && g_ascii_strncasecmp (p, "width", len) == 0) {
			NEW_COMPONENT (RSPAMD_HTML_COMPONENT_WIDTH);
		}
		else if (len == 6 && g_ascii_strncasecmp (p, "height", len) == 0) {
			NEW_COMPONENT (RSPAMD_HTML_COMPONENT_HEIGHT);
		}
		else if (g_ascii_strncasecmp (p, "style", len) == 0) {
			NEW_COMPONENT (RSPAMD_HTML_COMPONENT_STYLE);
		}
	}
	else if (tag->flags & FL_BLOCK) {
		if (len == 5){
			if (g_ascii_strncasecmp (p, "color", len) == 0) {
				NEW_COMPONENT (RSPAMD_HTML_COMPONENT_COLOR);
			}
			else if (g_ascii_strncasecmp (p, "style", len) == 0) {
				NEW_COMPONENT (RSPAMD_HTML_COMPONENT_STYLE);
			}
			else if (g_ascii_strncasecmp (p, "class", len) == 0) {
				NEW_COMPONENT (RSPAMD_HTML_COMPONENT_CLASS);
			}
		}
		else if (len == 7) {
			if (g_ascii_strncasecmp (p, "bgcolor", len) == 0) {
				NEW_COMPONENT (RSPAMD_HTML_COMPONENT_BGCOLOR);
			}
		}
	}

	return ret;
}

static void
rspamd_html_parse_tag_content (rspamd_mempool_t *pool,
		struct html_content *hc, struct html_tag *tag, const guchar *in,
		gint *statep, guchar const **savep)
{
	enum {
		parse_start = 0,
		parse_name,
		parse_attr_name,
		parse_equal,
		parse_start_dquote,
		parse_dqvalue,
		parse_end_dquote,
		parse_start_squote,
		parse_sqvalue,
		parse_end_squote,
		parse_value,
		spaces_after_name,
		spaces_before_eq,
		spaces_after_eq,
		spaces_after_param,
		ignore_bad_tag
	} state;
	struct html_tag_def *found;
	gboolean store = FALSE;
	struct html_tag_component *comp;

	state = *statep;

	switch (state) {
	case parse_start:
		if (!g_ascii_isalpha (*in) && !g_ascii_isspace (*in)) {
			hc->flags |= RSPAMD_HTML_FLAG_BAD_ELEMENTS;
			state = ignore_bad_tag;
			tag->id = -1;
			tag->flags |= FL_BROKEN;
		}
		else if (g_ascii_isalpha (*in)) {
			state = parse_name;
			tag->name.start = in;
		}
		break;

	case parse_name:
		if (g_ascii_isspace (*in) || *in == '>' || *in == '/') {
			g_assert (in >= tag->name.start);

			if (*in == '/') {
				tag->flags |= FL_CLOSED;
			}

			tag->name.len = in - tag->name.start;

			if (tag->name.len == 0) {
				hc->flags |= RSPAMD_HTML_FLAG_BAD_ELEMENTS;
				tag->id = -1;
				tag->flags |= FL_BROKEN;
				state = ignore_bad_tag;
			}
			else {
				gchar *s;
				/* We CANNOT safely modify tag's name here, as it is already parsed */

				s = rspamd_mempool_alloc (pool, tag->name.len);
				memcpy (s, tag->name.start, tag->name.len);
				tag->name.len = rspamd_html_decode_entitles_inplace (s,
						tag->name.len);
				tag->name.start = s;

				found = bsearch (tag, tag_defs, G_N_ELEMENTS (tag_defs),
					sizeof (tag_defs[0]), tag_find);
				if (found == NULL) {
					hc->flags |= RSPAMD_HTML_FLAG_UNKNOWN_ELEMENTS;
					tag->id = -1;
				}
				else {
					tag->id = found->id;
					tag->flags = found->flags;
				}
				state = spaces_after_name;
			}
		}
		break;

	case parse_attr_name:
		if (*savep == NULL) {
			state = ignore_bad_tag;
		}
		else {
			if (*in == '=') {
				state = parse_equal;
			}
			else if (g_ascii_isspace (*in)) {
				state = spaces_before_eq;
			}
			else if (*in == '/') {
				tag->flags |= FL_CLOSED;
			}
			else {
				return;
			}

			if (!rspamd_html_parse_tag_component (pool, *savep, in, tag)) {
				/* Ignore unknown params */
				*savep = NULL;
			}
		}

		break;

	case spaces_after_name:
		if (!g_ascii_isspace (*in)) {
			*savep = in;
			if (*in == '/') {
				tag->flags |= FL_CLOSED;
			}
			else if (*in != '>') {
				state = parse_attr_name;
			}
		}
		break;

	case spaces_before_eq:
		if (*in == '=') {
			state = parse_equal;
		}
		else if (!g_ascii_isspace (*in)) {
			hc->flags |= RSPAMD_HTML_FLAG_BAD_ELEMENTS;
			tag->flags |= FL_BROKEN;
			state = ignore_bad_tag;
		}
		break;

	case spaces_after_eq:
		if (*in == '"') {
			state = parse_start_dquote;
		}
		else if (*in == '\'') {
			state = parse_start_squote;
		}
		else if (!g_ascii_isspace (*in)) {
			if (*savep != NULL) {
				/* We need to save this param */
				*savep = in;
			}
			state = parse_value;
		}
		break;

	case parse_equal:
		if (g_ascii_isspace (*in)) {
			state = spaces_after_eq;
		}
		else if (*in == '"') {
			state = parse_start_dquote;
		}
		else if (*in == '\'') {
			state = parse_start_squote;
		}
		else {
			if (*savep != NULL) {
				/* We need to save this param */
				*savep = in;
			}
			state = parse_value;
		}
		break;

	case parse_start_dquote:
		if (*in == '"') {
			if (*savep != NULL) {
				/* We have an empty attribute value */
				savep = NULL;
			}
			state = spaces_after_param;
		}
		else {
			if (*savep != NULL) {
				/* We need to save this param */
				*savep = in;
			}
			state = parse_dqvalue;
		}
		break;

	case parse_start_squote:
		if (*in == '\'') {
			if (*savep != NULL) {
				/* We have an empty attribute value */
				savep = NULL;
			}
			state = spaces_after_param;
		}
		else {
			if (*savep != NULL) {
				/* We need to save this param */
				*savep = in;
			}
			state = parse_sqvalue;
		}
		break;

	case parse_dqvalue:
		if (*in == '"') {
			store = TRUE;
			state = parse_end_dquote;
		}
		if (store) {
			if (*savep != NULL) {
				gchar *s;

				g_assert (tag->params != NULL);
				comp = g_queue_peek_tail (tag->params);
				g_assert (comp != NULL);
				comp->len = in - *savep;
				s = rspamd_mempool_alloc (pool, comp->len);
				memcpy (s, *savep, comp->len);
				comp->len = rspamd_html_decode_entitles_inplace (s, comp->len);
				comp->start = s;
				*savep = NULL;
			}
		}
		break;

	case parse_sqvalue:
		if (*in == '\'') {
			store = TRUE;
			state = parse_end_squote;
		}
		if (store) {
			if (*savep != NULL) {
				gchar *s;

				g_assert (tag->params != NULL);
				comp = g_queue_peek_tail (tag->params);
				g_assert (comp != NULL);
				comp->len = in - *savep;
				s = rspamd_mempool_alloc (pool, comp->len);
				memcpy (s, *savep, comp->len);
				comp->len = rspamd_html_decode_entitles_inplace (s, comp->len);
				comp->start = s;
				*savep = NULL;
			}
		}
		break;

	case parse_value:
		if (*in == '/' && *(in + 1) == '>') {
			tag->flags |= FL_CLOSED;
			store = TRUE;
		}
		else if (g_ascii_isspace (*in) || *in == '>') {
			store = TRUE;
			state = spaces_after_param;
		}

		if (store) {
			if (*savep != NULL) {
				gchar *s;

				g_assert (tag->params != NULL);
				comp = g_queue_peek_tail (tag->params);
				g_assert (comp != NULL);
				comp->len = in - *savep;
				s = rspamd_mempool_alloc (pool, comp->len);
				memcpy (s, *savep, comp->len);
				comp->len = rspamd_html_decode_entitles_inplace (s, comp->len);
				comp->start = s;
				*savep = NULL;
			}
		}
		break;

	case parse_end_dquote:
	case parse_end_squote:
		if (g_ascii_isspace (*in)) {
			state = spaces_after_param;
		}
		else if (*in == '/' && *(in + 1) == '>') {
			tag->flags |= FL_CLOSED;
		}
		break;

	case spaces_after_param:
		if (!g_ascii_isspace (*in)) {
			if (*in == '/' && *(in + 1) == '>') {
				tag->flags |= FL_CLOSED;
			}

			state = parse_attr_name;
			*savep = in;
		}
		break;

	case ignore_bad_tag:
		break;
	}

	*statep = state;
}



struct rspamd_url *
rspamd_html_process_url (rspamd_mempool_t *pool, const gchar *start, guint len,
		struct html_tag_component *comp)
{
	struct rspamd_url *url;
	gchar *decoded;
	gint rc;
	gsize decoded_len;
	const gchar *p, *s;
	gchar *d;
	guint i, dlen;
	gboolean has_bad_chars = FALSE, no_prefix = FALSE;
	static const gchar hexdigests[16] = "0123456789abcdef";

	p = start;

	/* Strip spaces from the url */
	/* Head spaces */
	while ( p < start + len && g_ascii_isspace (*p)) {
		p ++;
		start ++;
		len --;
	}

	if (comp) {
		comp->start = p;
		comp->len = len;
	}

	/* Trailing spaces */
	p = start + len - 1;

	while (p >= start && g_ascii_isspace (*p)) {
		p --;
		len --;

		if (comp) {
			comp->len --;
		}
	}

	s = start;
	dlen = 0;

	for (i = 0; i < len; i ++) {
		if (G_UNLIKELY (((guint)s[i]) < 0x80 && !g_ascii_isgraph (s[i]))) {
			dlen += 3;
		}
		else {
			dlen ++;
		}
	}

	if (memchr (s, ':', len) == NULL) {
		/* We have no prefix */
		dlen += sizeof ("http://") - 1;
		no_prefix = TRUE;
	}

	decoded = rspamd_mempool_alloc (pool, dlen + 1);
	d = decoded;

	if (no_prefix) {
		if (s[0] == '/' && (len > 2 && s[1] == '/')) {
			/* //bla case */
			memcpy (d, "http:", sizeof ("http:") - 1);
			d += sizeof ("http:") - 1;
		}
		else {
			memcpy (d, "http://", sizeof ("http://") - 1);
			d += sizeof ("http://") - 1;
		}
	}

	/* We also need to remove all internal newlines and encode unsafe characters */
	for (i = 0; i < len; i ++) {
		if (G_UNLIKELY (s[i] == '\r' || s[i] == '\n')) {
			continue;
		}
		else if (G_UNLIKELY (((guint)s[i]) < 0x80 && !g_ascii_isgraph (s[i]))) {
			/* URL encode */
			*d++ = '%';
			*d++ = hexdigests[(s[i] >> 4) & 0xf];
			*d++ = hexdigests[s[i] & 0xf];
			has_bad_chars = TRUE;
		}
		else {
			*d++ = s[i];
		}
	}

	*d = '\0';
	dlen = d - decoded;

	url = rspamd_mempool_alloc0 (pool, sizeof (*url));

	if (rspamd_normalise_unicode_inplace (pool, decoded, &dlen)) {
		url->flags |= RSPAMD_URL_FLAG_UNNORMALISED;
	}

	rc = rspamd_url_parse (url, decoded, dlen, pool);

	if (rc == URI_ERRNO_OK) {
		if (has_bad_chars) {
			url->flags |= RSPAMD_URL_FLAG_OBSCURED;
		}

		if (no_prefix) {
			url->flags |= RSPAMD_URL_FLAG_SCHEMALESS;
		}

		decoded = url->string;
		decoded_len = url->urllen;

		if (comp) {
			comp->start = decoded;
			comp->len = decoded_len;
		}
		/* Spaces in href usually mean an attempt to obfuscate URL */
		/* See https://github.com/vstakhov/rspamd/issues/593 */
#if 0
		if (has_spaces) {
			url->flags |= RSPAMD_URL_FLAG_OBSCURED;
		}
#endif

		return url;
	}

	return NULL;
}

static struct rspamd_url *
rspamd_html_process_url_tag (rspamd_mempool_t *pool, struct html_tag *tag)
{
	struct html_tag_component *comp;
	GList *cur;
	struct rspamd_url *url;

	cur = tag->params->head;

	while (cur) {
		comp = cur->data;

		if (comp->type == RSPAMD_HTML_COMPONENT_HREF && comp->len > 0) {
			url = rspamd_html_process_url (pool, comp->start, comp->len, comp);

			if (url && tag->extra == NULL) {
				tag->extra = url;
			}

			return url;
		}

		cur = g_list_next (cur);
	}

	return NULL;
}

static void
rspamd_process_html_url (rspamd_mempool_t *pool, struct rspamd_url *url,
		GHashTable *tbl_urls, GHashTable *tbl_emails)
{
	GHashTable *target_tbl;
	struct rspamd_url *query_url, *existing;
	gchar *url_str;
	gint rc;
	gboolean prefix_added;

	if (url->flags & RSPAMD_URL_FLAG_UNNORMALISED) {
		url->flags |= RSPAMD_URL_FLAG_OBSCURED;
	}

	if (url->querylen > 0) {

		if (rspamd_url_find (pool, url->query, url->querylen, &url_str, TRUE,
				NULL, &prefix_added)) {
			query_url = rspamd_mempool_alloc0 (pool,
					sizeof (struct rspamd_url));

			rc = rspamd_url_parse (query_url,
					url_str,
					strlen (url_str),
					pool);

			if (rc == URI_ERRNO_OK &&
					query_url->hostlen > 0) {
				msg_debug_html ("found url %s in query of url"
						" %*s", url_str, url->querylen, url->query);

				if (query_url->protocol == PROTOCOL_MAILTO) {
					target_tbl = tbl_emails;
				}
				else {
					target_tbl = tbl_urls;
				}

				if (prefix_added) {
					query_url->flags |= RSPAMD_URL_FLAG_SCHEMALESS;
				}

				if (query_url->flags
						& (RSPAMD_URL_FLAG_UNNORMALISED|RSPAMD_URL_FLAG_OBSCURED|
							RSPAMD_URL_FLAG_NUMERIC)) {
					/* Set obscured flag if query url is bad */
					url->flags |= RSPAMD_URL_FLAG_OBSCURED;
				}

				/* And vice-versa */
				if (url->flags & RSPAMD_URL_FLAG_OBSCURED) {
					query_url->flags |= RSPAMD_URL_FLAG_OBSCURED;
				}

				if ((existing = g_hash_table_lookup (target_tbl,
						query_url)) == NULL) {
					g_hash_table_insert (target_tbl,
							query_url,
							query_url);
				}
				else {
					existing->count ++;
				}
			}
		}
	}
}

static void
rspamd_html_process_img_tag (rspamd_mempool_t *pool, struct html_tag *tag,
		struct html_content *hc)
{
	struct html_tag_component *comp;
	struct html_image *img;
	rspamd_ftok_t fstr;
	const guchar *p;
	GList *cur;
	gulong val;
	gboolean seen_width = FALSE, seen_height = FALSE;
	goffset pos;

	cur = tag->params->head;
	img = rspamd_mempool_alloc0 (pool, sizeof (*img));
	img->tag = tag;

	while (cur) {
		comp = cur->data;

		if (comp->type == RSPAMD_HTML_COMPONENT_HREF && comp->len > 0) {
			fstr.begin = (gchar *)comp->start;
			fstr.len = comp->len;
			img->src = rspamd_mempool_ftokdup (pool, &fstr);

			if (comp->len > sizeof ("cid:") - 1 && memcmp (comp->start,
					"cid:", sizeof ("cid:") - 1) == 0) {
				/* We have an embedded image */
				img->flags |= RSPAMD_HTML_FLAG_IMAGE_EMBEDDED;
			}
			else {
				img->flags |= RSPAMD_HTML_FLAG_IMAGE_EXTERNAL;
			}
		}
		else if (comp->type == RSPAMD_HTML_COMPONENT_HEIGHT) {
			rspamd_strtoul (comp->start, comp->len, &val);
			img->height = val;
			seen_height = TRUE;
		}
		else if (comp->type == RSPAMD_HTML_COMPONENT_WIDTH) {
			rspamd_strtoul (comp->start, comp->len, &val);
			img->width = val;
			seen_width = TRUE;
		}
		else if (comp->type == RSPAMD_HTML_COMPONENT_STYLE) {
			/* Try to search for height= or width= in style tag */
			if (!seen_height && comp->len > 0) {
				pos = rspamd_substring_search_caseless (comp->start, comp->len,
						"height", sizeof ("height") - 1);

				if (pos != -1) {
					p = comp->start + pos + sizeof ("height") - 1;

					while (p < comp->start + comp->len) {
						if (g_ascii_isdigit (*p)) {
							rspamd_strtoul (p, comp->len - (p - comp->start), &val);
							img->height = val;
							break;
						}
						else if (!g_ascii_isspace (*p) && *p != '=' && *p != ':') {
							/* Fallback */
							break;
						}
						p ++;
					}
				}
			}

			if (!seen_width && comp->len > 0) {
				pos = rspamd_substring_search_caseless (comp->start, comp->len,
						"width", sizeof ("width") - 1);

				if (pos != -1) {
					p = comp->start + pos + sizeof ("width") - 1;

					while (p < comp->start + comp->len) {
						if (g_ascii_isdigit (*p)) {
							rspamd_strtoul (p, comp->len - (p - comp->start), &val);
							img->width = val;
							break;
						}
						else if (!g_ascii_isspace (*p) && *p != '=' && *p != ':') {
							/* Fallback */
							break;
						}
						p ++;
					}
				}
			}
		}

		cur = g_list_next (cur);
	}

	if (hc->images == NULL) {
		hc->images = g_ptr_array_sized_new (4);
		rspamd_mempool_add_destructor (pool, rspamd_ptr_array_free_hard,
				hc->images);
	}

	g_ptr_array_add (hc->images, img);
	tag->extra = img;
}

static void
rspamd_html_process_color (const gchar *line, guint len, struct html_color *cl)
{
	const gchar *p = line, *end = line + len;
	char hexbuf[7];
	rspamd_ftok_t search;
	struct html_color *el;

	memset (cl, 0, sizeof (*cl));

	if (*p == '#') {
		/* HEX color */
		p ++;
		rspamd_strlcpy (hexbuf, p, MIN ((gint)sizeof(hexbuf), end - p + 1));
		cl->d.val = strtoul (hexbuf, NULL, 16);
		cl->valid = TRUE;
	}
	else if (len > 4 && rspamd_lc_cmp (p, "rgb", 3) == 0) {
		/* We have something like rgba(x,x,x,x) or rgb(x,x,x) */
		enum {
			obrace,
			num1,
			num2,
			num3,
			skip_spaces
		} state = skip_spaces, next_state = obrace;
		gulong r = 0, g = 0, b = 0;
		const gchar *c;
		gboolean valid = FALSE;

		p += 3;

		if (*p == 'a') {
			p ++;
		}

		c = p;

		while (p < end) {
			switch (state) {
			case obrace:
				if (*p == '(') {
					p ++;
					state = skip_spaces;
					next_state = num1;
				}
				else if (g_ascii_isspace (*p)) {
					state = skip_spaces;
					next_state = obrace;
				}
				else {
					goto stop;
				}
				break;
			case num1:
				if (*p == ',') {
					if (!rspamd_strtoul (c, p - c, &r)) {
						goto stop;
					}

					p ++;
					state = skip_spaces;
					next_state = num2;
				}
				else if (!g_ascii_isdigit (*p)) {
					goto stop;
				}
				else {
					p ++;
				}
				break;
			case num2:
				if (*p == ',') {
					if (!rspamd_strtoul (c, p - c, &g)) {
						goto stop;
					}

					p ++;
					state = skip_spaces;
					next_state = num3;
				}
				else if (!g_ascii_isdigit (*p)) {
					goto stop;
				}
				else {
					p ++;
				}
				break;
			case num3:
				if (*p == ',') {
					if (!rspamd_strtoul (c, p - c, &b)) {
						goto stop;
					}

					valid = TRUE;
					goto stop;
				}
				else if (!g_ascii_isdigit (*p)) {
					goto stop;
				}
				else {
					p ++;
				}
				break;
			case skip_spaces:
				if (!g_ascii_isspace (*p)) {
					c = p;
					state = next_state;
				}
				else {
					p ++;
				}
				break;
			}
		}

		stop:

		if (valid) {
			cl->d.val = b + (g << 8) + (r << 16);
			cl->valid = TRUE;
		}
	}
	else {
		/* Compare color by name */
		search.begin = line;
		search.len = len;

		el = g_hash_table_lookup (html_colors_hash, &search);

		if (el != NULL) {
			memcpy (cl, el, sizeof (*cl));
		}
	}
}

static void
rspamd_html_process_style (rspamd_mempool_t *pool, struct html_block *bl,
		struct html_content *hc, const gchar *style, guint len)
{
	const gchar *p, *c, *end, *key = NULL;
	enum {
		read_key,
		read_colon,
		read_value,
		skip_spaces,
	} state = skip_spaces, next_state = read_key;
	guint klen = 0;

	p = style;
	c = p;
	end = p + len;

	while (p <= end) {
		switch(state) {
		case read_key:
			if (p == end || *p == ':') {
				key = c;
				klen = p - c;
				state = skip_spaces;
				next_state = read_value;
			}
			else if (g_ascii_isspace (*p)) {
				key = c;
				klen = p - c;
				state = skip_spaces;
				next_state = read_colon;
			}

			p ++;
			break;

		case read_colon:
			if (p == end || *p == ':') {
				state = skip_spaces;
				next_state = read_value;
			}

			p ++;
			break;

		case read_value:
			if (p == end || *p == ';') {
				if (key && klen && p - c > 0) {
					if ((klen == 5 && g_ascii_strncasecmp (key, "color", 5) == 0)
					|| (klen == 10 && g_ascii_strncasecmp (key, "font-color", 10) == 0)) {

						rspamd_html_process_color (c, p - c, &bl->font_color);
						msg_debug_html ("got color: %xd", bl->font_color.d.val);
					}
					else if ((klen == 16 && g_ascii_strncasecmp (key,
							"background-color", 16) == 0) ||
							(klen == 10 && g_ascii_strncasecmp (key,
									"background", 10) == 0)) {

						rspamd_html_process_color (c, p - c, &bl->background_color);
						msg_debug_html ("got bgcolor: %xd", bl->background_color.d.val);
					}
					else if (klen == 7 && g_ascii_strncasecmp (key, "display", 7) == 0) {
						if (p - c >= 4 && rspamd_substring_search_caseless (c, p - c,
								"none", 4) != -1) {
							bl->visible = FALSE;
							msg_debug_html ("tag is not visible");
						}
					}
				}

				key = NULL;
				klen = 0;
				state = skip_spaces;
				next_state = read_key;
			}

			p ++;
			break;

		case skip_spaces:
			if (p < end && !g_ascii_isspace (*p)) {
				c = p;
				state = next_state;
			}
			else {
				p ++;
			}

			break;
		}
	}
}

static void
rspamd_html_process_block_tag (rspamd_mempool_t *pool, struct html_tag *tag,
		struct html_content *hc)
{
	struct html_tag_component *comp;
	struct html_block *bl, *bl_parent;
	rspamd_ftok_t fstr;
	GList *cur;
	GNode *parent;
	struct html_tag *parent_tag;

	cur = tag->params->head;
	bl = rspamd_mempool_alloc0 (pool, sizeof (*bl));
	bl->tag = tag;
	bl->visible = TRUE;

	while (cur) {
		comp = cur->data;

		if (comp->type == RSPAMD_HTML_COMPONENT_COLOR && comp->len > 0) {
			fstr.begin = (gchar *)comp->start;
			fstr.len = comp->len;
			rspamd_html_process_color (comp->start, comp->len, &bl->font_color);
			msg_debug_html ("got color: %xd", bl->font_color.d.val);
		}
		else if (comp->type == RSPAMD_HTML_COMPONENT_BGCOLOR && comp->len > 0) {
			fstr.begin = (gchar *)comp->start;
			fstr.len = comp->len;
			rspamd_html_process_color (comp->start, comp->len, &bl->background_color);
			msg_debug_html ("got color: %xd", bl->font_color.d.val);

			if (tag->id == Tag_BODY) {
				/* Set global background color */
				memcpy (&hc->bgcolor, &bl->background_color, sizeof (hc->bgcolor));
			}
		}
		else if (comp->type == RSPAMD_HTML_COMPONENT_STYLE && comp->len > 0) {
			bl->style.len = comp->len;
			bl->style.start =  comp->start;
			msg_debug_html ("got style: %*s", (gint)bl->style.len, bl->style.start);
			rspamd_html_process_style (pool, bl, hc, comp->start, comp->len);
		}
		else if (comp->type == RSPAMD_HTML_COMPONENT_CLASS && comp->len > 0) {
			fstr.begin = (gchar *)comp->start;
			fstr.len = comp->len;
			bl->class = rspamd_mempool_ftokdup (pool, &fstr);
			msg_debug_html ("got class: %s", bl->class);
		}

		cur = g_list_next (cur);
	}

	if (!bl->background_color.valid) {
		/* Try to propagate background color from parent nodes */
		for (parent = tag->parent; parent != NULL; parent = parent->parent) {
			parent_tag = parent->data;

			if (parent_tag && (parent_tag->flags & FL_BLOCK) && parent_tag->extra) {
				bl_parent = parent_tag->extra;

				if (bl_parent->background_color.valid) {
					memcpy (&bl->background_color, &bl_parent->background_color,
							sizeof (bl->background_color));
					break;
				}
			}
		}
	}
	if (!bl->font_color.valid) {
		/* Try to propagate background color from parent nodes */
		for (parent = tag->parent; parent != NULL; parent = parent->parent) {
			parent_tag = parent->data;

			if (parent_tag && (parent_tag->flags & FL_BLOCK) && parent_tag->extra) {
				bl_parent = parent_tag->extra;

				if (bl_parent->font_color.valid) {
					memcpy (&bl->font_color, &bl_parent->font_color,
							sizeof (bl->font_color));
					break;
				}
			}
		}
	}

	/* Set bgcolor to the html bgcolor and font color to black as a last resort */
	if (!bl->font_color.valid) {
		bl->font_color.d.val = 0;
		bl->font_color.d.comp.alpha = 255;
		bl->font_color.valid = TRUE;
	}
	if (!bl->background_color.valid) {
		memcpy (&bl->background_color, &hc->bgcolor, sizeof (hc->bgcolor));
	}

	if (hc->blocks == NULL) {
		hc->blocks = g_ptr_array_sized_new (64);
		rspamd_mempool_add_destructor (pool, rspamd_ptr_array_free_hard,
				hc->blocks);
	}

	g_ptr_array_add (hc->blocks, bl);
	tag->extra = bl;
}

static void
rspamd_html_check_displayed_url (rspamd_mempool_t *pool,
		GList **exceptions, GHashTable *urls, GHashTable *emails,
		GByteArray *dest, GHashTable *target_tbl,
		gint href_offset,
		struct rspamd_url *url)
{
	struct rspamd_url *displayed_url = NULL;
	struct rspamd_url *turl;
	gboolean url_found = FALSE;
	struct rspamd_process_exception *ex;

	if (href_offset <= 0) {
		/* No dispalyed url, just some text within <a> tag */
		return;
	}

	rspamd_html_url_is_phished (pool, url,
			dest->data + href_offset,
			dest->len - href_offset,
			&url_found, &displayed_url);

	if (exceptions && url_found) {
		ex = rspamd_mempool_alloc (pool,
				sizeof (*ex));
		ex->pos = href_offset;
		ex->len = dest->len - href_offset;
		ex->type = RSPAMD_EXCEPTION_URL;

		*exceptions = g_list_prepend (*exceptions,
				ex);
	}

	if (displayed_url) {
		if (displayed_url->protocol ==
				PROTOCOL_MAILTO) {
			target_tbl = emails;
		}
		else {
			target_tbl = urls;
		}

		if (target_tbl != NULL) {
			turl = g_hash_table_lookup (target_tbl,
					displayed_url);

			if (turl != NULL) {
				/* Here, we assume the following:
				 * if we have a URL in the text part which
				 * is the same as displayed URL in the
				 * HTML part, we assume that it is also
				 * hint only.
				 */
				if (turl->flags &
						RSPAMD_URL_FLAG_FROM_TEXT) {
					turl->flags |= RSPAMD_URL_FLAG_HTML_DISPLAYED;
					turl->flags &= ~RSPAMD_URL_FLAG_FROM_TEXT;
				}

				turl->count ++;
			}
			else {
				g_hash_table_insert (target_tbl,
						displayed_url,
						displayed_url);
			}
		}
	}
}

GByteArray*
rspamd_html_process_part_full (rspamd_mempool_t *pool, struct html_content *hc,
		GByteArray *in, GList **exceptions, GHashTable *urls,  GHashTable *emails)
{
	const guchar *p, *c, *end, *savep = NULL;
	guchar t;
	gboolean closing = FALSE, need_decode = FALSE, save_space = FALSE,
			balanced;
	GByteArray *dest;
	GHashTable *target_tbl;
	guint obrace = 0, ebrace = 0;
	GNode *cur_level = NULL;
	gint substate = 0, len, href_offset = -1;
	struct html_tag *cur_tag = NULL, *content_tag = NULL;
	struct rspamd_url *url = NULL, *turl;
	enum {
		parse_start = 0,
		tag_begin,
		sgml_tag,
		xml_tag,
		compound_tag,
		comment_tag,
		comment_content,
		sgml_content,
		tag_content,
		tag_end,
		xml_tag_end,
		content_ignore,
		content_write,
		content_ignore_sp
	} state = parse_start;

	g_assert (in != NULL);
	g_assert (hc != NULL);
	g_assert (pool != NULL);

	rspamd_html_library_init ();
	hc->tags_seen = rspamd_mempool_alloc0 (pool, NBYTES (G_N_ELEMENTS (tag_defs)));

	/* Set white background color by default */
	hc->bgcolor.d.comp.alpha = 0;
	hc->bgcolor.d.comp.r = 255;
	hc->bgcolor.d.comp.g = 255;
	hc->bgcolor.d.comp.b = 255;
	hc->bgcolor.valid = TRUE;

	dest = g_byte_array_sized_new (in->len / 3 * 2);

	p = in->data;
	c = p;
	end = p + in->len;

	while (p < end) {
		t = *p;

		switch (state) {
		case parse_start:
			if (t == '<') {
				state = tag_begin;
			}
			else {
				/* We have no starting tag, so assume that it's content */
				hc->flags |= RSPAMD_HTML_FLAG_BAD_START;
				state = content_write;
			}

			break;
		case tag_begin:
			switch (t) {
			case '<':
				p ++;
				closing = FALSE;
				break;
			case '!':
				state = sgml_tag;
				p ++;
				break;
			case '?':
				state = xml_tag;
				hc->flags |= RSPAMD_HTML_FLAG_XML;
				p ++;
				break;
			case '/':
				closing = TRUE;
				p ++;
				break;
			case '>':
				/* Empty tag */
				hc->flags |= RSPAMD_HTML_FLAG_BAD_ELEMENTS;
				state = tag_end;
				p ++;
				break;
			default:
				state = tag_content;
				substate = 0;
				savep = NULL;
				cur_tag = rspamd_mempool_alloc0 (pool, sizeof (*cur_tag));
				cur_tag->params = g_queue_new ();
				rspamd_mempool_add_destructor (pool,
						(rspamd_mempool_destruct_t)g_queue_free, cur_tag->params);
				break;
			}

			break;

		case sgml_tag:
			switch (t) {
			case '[':
				state = compound_tag;
				obrace = 1;
				ebrace = 0;
				p ++;
				break;
			case '-':
				state = comment_tag;
				p ++;
				break;
			default:
				state = sgml_content;
				break;
			}

			break;

		case xml_tag:
			if (t == '?') {
				state = xml_tag_end;
			}
			else if (t == '>') {
				/* Misformed xml tag */
				hc->flags |= RSPAMD_HTML_FLAG_BAD_ELEMENTS;
				state = tag_end;
				continue;
			}
			/* We efficiently ignore xml tags */
			p ++;
			break;

		case xml_tag_end:
			if (t == '>') {
				state = tag_end;
			}
			else {
				hc->flags |= RSPAMD_HTML_FLAG_BAD_ELEMENTS;
				p ++;
			}
			break;

		case compound_tag:
			if (t == '[') {
				obrace ++;
			}
			else if (t == ']') {
				ebrace ++;
			}
			else if (t == '>' && obrace == ebrace) {
				state = tag_end;
			}
			p ++;
			break;

		case comment_tag:
			if (t != '-')  {
				hc->flags |= RSPAMD_HTML_FLAG_BAD_ELEMENTS;
			}
			p ++;
			ebrace = 0;
			state = comment_content;
			break;

		case comment_content:
			if (t == '-') {
				ebrace ++;
			}
			else if (t == '>' && ebrace == 2) {
				state = tag_end;
				continue;
			}
			else {
				ebrace = 0;
			}

			p ++;
			break;

		case content_ignore:
			if (t != '<') {
				p ++;
			}
			else {
				state = tag_begin;
			}
			break;

		case content_write:

			if (t != '<') {
				if (t == '&') {
					need_decode = TRUE;
				}
				else if (g_ascii_isspace (t)) {
					save_space = TRUE;

					if (p > c) {
						if (need_decode) {
							goffset old_offset = dest->len;

							g_byte_array_append (dest, c, (p - c));

							len = rspamd_html_decode_entitles_inplace (
									dest->data + old_offset,
									p - c);
							dest->len = dest->len + len - (p - c);
						}
						else {
							len = p - c;
							g_byte_array_append (dest, c, len);
						}

						if (content_tag) {
							if (content_tag->content == NULL) {
								content_tag->content = c;
							}

							content_tag->content_length += p - c + 1;
						}
					}

					c = p;
					state = content_ignore_sp;
				}
				else {
					if (save_space) {
						/* Append one space if needed */
						if (dest->len > 0 &&
								!g_ascii_isspace (dest->data[dest->len - 1])) {
							g_byte_array_append (dest, " ", 1);
						}
						save_space = FALSE;
					}
				}
			}
			else {
				if (c != p) {

					if (need_decode) {
						goffset old_offset = dest->len;

						g_byte_array_append (dest, c, (p - c));
						len = rspamd_html_decode_entitles_inplace (
								dest->data + old_offset,
								p - c);
						dest->len = dest->len + len - (p - c);
					}
					else {
						len = p - c;
						g_byte_array_append (dest, c, len);
					}


					if (content_tag) {
						if (content_tag->content == NULL) {
							content_tag->content = c;
						}

						content_tag->content_length += p - c;
					}
				}

				content_tag = NULL;

				state = tag_begin;
				continue;
			}

			p ++;
			break;

		case content_ignore_sp:
			if (!g_ascii_isspace (t)) {
				c = p;
				state = content_write;
				continue;
			}

			if (content_tag) {
				content_tag->content_length ++;
			}

			p ++;
			break;

		case sgml_content:
			/* TODO: parse DOCTYPE here */
			if (t == '>') {
				state = tag_end;
				/* We don't know a lot about sgml tags, ignore them */
				cur_tag = NULL;
				continue;
			}
			p ++;
			break;

		case tag_content:
			rspamd_html_parse_tag_content (pool, hc, cur_tag,
					p, &substate, &savep);
			if (t == '>') {
				if (closing) {
					cur_tag->flags |= FL_CLOSING;

					if (cur_tag->flags & FL_CLOSED) {
						/* Bad mix of closed and closing */
						hc->flags |= RSPAMD_HTML_FLAG_BAD_ELEMENTS;
					}

					closing = FALSE;
				}

				state = tag_end;
				continue;
			}
			p ++;
			break;

		case tag_end:
			substate = 0;
			savep = NULL;

			if (cur_tag != NULL) {
				balanced = TRUE;

				if (rspamd_html_process_tag (pool, hc, cur_tag, &cur_level,
						&balanced)) {
					state = content_write;
					need_decode = FALSE;
				}
				else {
					state = content_ignore;
				}

				if (cur_tag->id != -1 && cur_tag->id < N_TAGS) {
					if (cur_tag->flags & CM_UNIQUE) {
						if (isset (hc->tags_seen, cur_tag->id)) {
							/* Duplicate tag has been found */
							hc->flags |= RSPAMD_HTML_FLAG_DUPLICATE_ELEMENTS;
						}
					}
					setbit (hc->tags_seen, cur_tag->id);
				}

				if (!(cur_tag->flags & (FL_CLOSED|FL_CLOSING))) {
					content_tag = cur_tag;
				}

				/* Handle newlines */
				if (cur_tag->id == Tag_BR || cur_tag->id == Tag_HR) {
					if (dest->len > 0 && dest->data[dest->len - 1] != '\n') {
						g_byte_array_append (dest, "\r\n", 2);
					}
					save_space = FALSE;
				}
				else if ((cur_tag->flags & (FL_CLOSED|FL_CLOSING)) &&
						(cur_tag->id == Tag_P ||
						cur_tag->id == Tag_TR ||
						cur_tag->id == Tag_DIV) && balanced) {
					if (dest->len > 0 && dest->data[dest->len - 1] != '\n') {
						g_byte_array_append (dest, "\r\n", 2);
					}
					save_space = FALSE;
				}

				if (cur_tag->id == Tag_A || cur_tag->id == Tag_IFRAME) {
					if (!(cur_tag->flags & (FL_CLOSING))) {
						url = rspamd_html_process_url_tag (pool, cur_tag);

						if (url != NULL) {

							if (url->protocol == PROTOCOL_MAILTO) {
								target_tbl = emails;
							}
							else {
								target_tbl = urls;
							}

							if (target_tbl != NULL) {
								turl = g_hash_table_lookup (target_tbl, url);

								if (turl == NULL) {
									g_hash_table_insert (target_tbl, url, url);
								}
								else {
									turl->count ++;
									url = NULL;
								}

								if (turl == NULL && url != NULL) {
									rspamd_process_html_url (pool,
											url,
											urls, emails);
								}
							}

							href_offset = dest->len;
						}
					}

					if (cur_tag->id == Tag_A) {
						if (!balanced && cur_level && cur_level->prev) {
							struct html_tag *prev_tag;
							struct rspamd_url *prev_url;

							prev_tag = cur_level->prev->data;

							if (prev_tag->id == Tag_A &&
									!(prev_tag->flags & (FL_CLOSING)) &&
									prev_tag->extra) {
								prev_url = prev_tag->extra;

								rspamd_html_check_displayed_url (pool,
										exceptions, urls, emails,
										dest, target_tbl, href_offset,
										prev_url);
							}
						}

						if (cur_tag->flags & (FL_CLOSING)) {

							/* Insert exception */
							if (url != NULL && (gint) dest->len > href_offset) {
								rspamd_html_check_displayed_url (pool,
										exceptions, urls, emails,
										dest, target_tbl, href_offset,
										url);

							}

							href_offset = -1;
							url = NULL;
						}
					}
				}
				else if (cur_tag->id == Tag_LINK) {
					url = rspamd_html_process_url_tag (pool, cur_tag);
				}

				if (cur_tag->id == Tag_IMG && !(cur_tag->flags & FL_CLOSING)) {
					rspamd_html_process_img_tag (pool, cur_tag, hc);
				}
				else if (!(cur_tag->flags & FL_CLOSING) &&
						(cur_tag->flags & FL_BLOCK)) {
					struct html_block *bl;

					rspamd_html_process_block_tag (pool, cur_tag, hc);
					bl = cur_tag->extra;

					if (bl && !bl->visible) {
						state = content_ignore;
					}
				}
			}
			else {
				state = content_write;
			}


			p++;
			c = p;
			cur_tag = NULL;
			break;
		}
	}

	return dest;
}

GByteArray*
rspamd_html_process_part (rspamd_mempool_t *pool,
		struct html_content *hc,
		GByteArray *in)
{
	return rspamd_html_process_part_full (pool, hc, in, NULL, NULL, NULL);
}
