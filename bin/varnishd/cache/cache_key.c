//
// http://tools.ietf.org/html/draft-fielding-http-key-00
//

//#include "config.h"

#include "cache.h"

#include "vct.h"
#include "vend.h"

#define M_WORD		1
#define M_SUBSTRING	2
#define M_BEGINNING	3
#define M_CASE		4
#define M_NOT		5

int
key_ParseMatcher(const char *s, struct vsb **sb) {
	const char *p = s;
	const char *e;
	while (*p == ';') {
		if (*p != ';')
			return -1;

		p++;

		if (strncmp(p, "w=\"", 3) == 0) {
			p += 3;
			for (e = p; *e != '\"'; e++)
				continue;
			VSB_printf(*sb, "%c%.*s%c", M_WORD, (int)(e -p), p, 0);
			p = e + 1;
		} else if (strncmp(p, "s=\"", 3) == 0) {
			p += 3;
			for (e = p; *e != '\"'; e++)
				continue;
			VSB_printf(*sb, "%c%.*s%c", M_SUBSTRING, (int)(e -p), p, 0);
			p = e + 1;
		} else if (strncmp(p, "b=\"", 3) == 0) {
			p += 3;
			for (e = p; *e != '\"'; e++)
				continue;
			VSB_printf(*sb, "%c%.*s%c", M_BEGINNING, (int)(e -p), p, 0);
			p = e + 1;
		} else if (*p == 'c') {
			VSB_printf(*sb, "%c", M_CASE);
			p++;
		} else if (*p == 'n') {
			VSB_printf(*sb, "%c", M_NOT);
			p++;
		} else {
			// Invalid matcher
			return -1;
		}
	}
	return p - s;
}

/*
 * Find length of a key entry
 */
static unsigned
key_len(const uint8_t *p)
{
	unsigned l = vbe16dec(p);

	return (3 + p[3] + 2 + (l == 0xffff ? 0 : l));
}

int
KEY_Create(struct busyobj *bo, struct vsb **psb)
{
	char *v, *h, *e, *p, *q;
	struct vsb *sb, *sbh, *sbm;
	unsigned l;
	int matcher = 0;
	int error = 0;

	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);
	CHECK_OBJ_NOTNULL(bo->bereq, HTTP_MAGIC);
	CHECK_OBJ_NOTNULL(bo->beresp, HTTP_MAGIC);
	AN(psb);
	AZ(*psb);

	if (!http_GetHdr(bo->beresp, "\4Key:", &v))
		return (0);

	sb = VSB_new_auto();
	AN(sb);

	/* For header matching strings */
	sbh = VSB_new_auto();
	AN(sbh);

	/* For matcher strings */
	sbm = VSB_new_auto();
	AN(sbm);

	for (p = v; *p; p++) {
		/* Find next header-name */
		if (vct_issp(*p))
			continue;
		for (q = p; *q && !vct_issp(*q) && *q != ',' && *q != ';'; q++)
			continue;

		if (q - p > INT8_MAX) {
			VSLb(bo->vsl, SLT_Error,
			    "Key header name length exceeded");
			error = 1;
			break;
		}

		/* Build header matching string */
		VSB_clear(sbh);
		VSB_printf(sbh, "%c%.*s:%c",
		    (char)(1 + (q - p)), (int)(q - p), p, 0);
		AZ(VSB_finish(sbh));

		if (*q == ';') {
			VSB_clear(sbm);
			int ksize = key_ParseMatcher(q, &sbm);
			if (ksize > 0)
			    q += ksize;
			else {
			    // TODO: Cleanup allocations
			    error = 1;
			    return 0;
			}
			AZ(VSB_finish(sbm));
			l = VSB_len(sbm);
			e = h;
			matcher = 1;
		} else {
			matcher = 0;
			if (http_GetHdr(bo->bereq, VSB_data(sbh), &h)) {
				AZ(vct_issp(*h));
				/* Trim trailing space */
				e = strchr(h, '\0');
				while (e > h && vct_issp(e[-1]))
					e--;
				/* Encode two byte length and contents */
				l = e - h;
				if (l > 0xffff - 1) {
					VSLb(bo->vsl, SLT_Error,
					    "Vary header maximum length exceeded");
					error = 1;
					break;
				}
			} else {
				e = h;
				l = 0xffff;
			}
		}

		VSB_printf(sb, "%c%c", (int)(l >> 8), (int)(l & 0xff));
		VSB_printf(sb, "%c", matcher);
		VSB_bcat(sb, VSB_data(sbh), VSB_len(sbh));
		if (e != h)
			VSB_bcat(sb, h, e - h);
		if (matcher == 1)
			VSB_bcat(sb, VSB_data(sbm), VSB_len(sbm));

		if (*q == '\0')
			break;
		if (*q != ',') {
			VSLb(bo->vsl, SLT_Error, "Malformed Key header");
			error = 1;
			break;
		}

		p = q;
	}

	if (error) {
		VSB_delete(sbh);
		VSB_delete(sb);
		return (-1);
	}

	/* Terminate key matching string */
	VSB_printf(sb, "%c%c%c%c", 0xff, 0xff, 0, 0);

	VSB_delete(sbh);
	AZ(VSB_finish(sb));

	if (KEY_Match(bo->bereq, VSB_data(sb)) == 0) {
	    return -1;
	}

	*psb = sb;
	return (VSB_len(sb));
}

void
KEY_Validate(const uint8_t *key)
{
	while (key[3] != 0) {
		assert(strlen((const char*)key+4) == key[3]);
		key += key_len(key);
	}
}

int enum_fields(const char *fv, const char **m, int *s) {
    const char *f = 0, *b = 0;
    enum state {
	start,
	skip_space,
	scan,
	scan_quote,
	scan_back,
	done,
	end
    } cur_state = start;

    do {
	switch (cur_state) {
	    case start:
		f = fv;
		cur_state = skip_space;
		break;
	    case skip_space:
		if (*f == '\0')
		    cur_state = end;
		else if (*f == ' ' || *f == ',')
		    f++;
		else if (*f == '"') {
		    b = f;
		    cur_state = scan_quote;
		} else {
		    b = f;
		    cur_state = scan;
		}
		break;
	    case scan:
		b++;
		if (*b == '\0')
		    cur_state = scan_back;
		else if (*b == '"')
		    cur_state = scan_quote;
		else if (*b == ',')
		    cur_state = scan_back;
		break;
	    case scan_quote:
		b++;
		if (*b == '\0')
		    cur_state = scan_back;
		else if (*b == '\\' && b[1] != '\0')
		    b += 2;
		else if (*b == '"')
		    cur_state = scan;
		break;
	    case scan_back:
		b--;
		if (*b != ' ')
		    cur_state = done;
		break;
	    case done:
		*m = f;
		*s = (int)(b-f)+1;
		return b - fv + 1;
		break;
	}
    } while (cur_state != end);

    return 0;
}

int cmp_func(const char *str1, const char *str2, int size, int case_sensitive) {
  if (case_sensitive == 1)
    return strncmp(str1, str2, size);
  else
    return strncasecmp(str1, str2, size);
}

int word_matcher(const char *p, const char *fv, int case_sensitive) {
    int read, size, offset = 0;
    const char *match;

    while (read = enum_fields(fv + offset, &match, &size)) {
	if (strlen(p) == size) {
	    if (cmp_func(match, p, size, case_sensitive) == 0) {
		return 1;
	    }
	}
	offset += read;
    }
    return 0;
}

int substring_matcher(const char *p, const char *fv, int case_sensitive) {
    int read, size, offset = 0;
    const char *match;

    while (read = enum_fields(fv + offset, &match, &size)) {
	if (strlen(p) <= size) {
	    const char *s;
	    for (s = match; s <= match + size - strlen(p); s++) {
		if (cmp_func(s, p, strlen(p), case_sensitive) == 0) {
		    return 1;
		}
	    }
	}
	offset += read;
    }
    return 0;
}

int beginning_substring_matcher(const char *p, const char *fv, int case_sensitive) {
    int read, size, offset = 0;
    const char *match;

    while (read = enum_fields(fv + offset, &match, &size)) {
	if (strlen(p) <= size) {
	    if (cmp_func(match, p, strlen(p), case_sensitive) == 0) {
		return 1;
	    }
	}
	offset += read;
    }
    return 0;
}

int
KEY_Match(struct http *http, const uint8_t *key)
{
	char *h;
	int i;
	int result = 1;

	while (key[3]) {
		// Exception for gzip
		if (cache_param->http_gzip_support &&
		    !strcasecmp(H_Accept_Encoding, (const char*) key + 3)) {
		// Exact match
		} else if (key[2] == 0) {
			char *e;
			unsigned l = vbe16dec(key);

			i = http_GetHdr(http, (const char*)(key+3), &h);

			if (l == 0xFFFF) {
			    if (i != 0)
				result = 0;
			} else {
			    const char *value = key + 4 + key[3] + 1;

			    if (i == 0) {
				result = 0;
			    } else {
				AZ(vct_issp(*h));
				/* Trim trailing space */
				e = strchr(h, '\0');
				while (e > h && vct_issp(e[-1]))
					e--;

				if (l != (int)(e - h))
				    result = 0;
				else
				    if (memcmp(h, value, l) != 0)
					result = 0;
			    }
			}

		// Matcher match
		} else if (key[2] == 1) {
			char *e;
			unsigned l = vbe16dec(key);
			int not_flag = 1;
			int case_flag = 0;

			i = http_GetHdr(http, (const char*)(key+3), &h);

			// TODO: Perhaps not matcher should allow this
			if (i == 0)
			    return 0;

			const char *matcher = key + 4 + key[3] + 1;

			while (*matcher != 0 && *matcher != -1) {
			    if (*matcher == M_WORD) {
				if (word_matcher(matcher + 1, h, case_flag) ^ not_flag)
				    result = 0;
				matcher += strlen(matcher) + 1;
			    } else if (*matcher == M_SUBSTRING) {
				if (substring_matcher(matcher + 1, h, case_flag) ^ not_flag)
				    result = 0;
				matcher += strlen(matcher) + 1;
			    } else if (*matcher == M_BEGINNING) {
				if (beginning_substring_matcher(matcher + 1, h, case_flag) ^ not_flag)
				    result = 0;
				matcher += strlen(matcher) + 1;
			    } else if (*matcher == M_CASE) {
				case_flag = 1;
				matcher++;
			    } else if (*matcher == M_NOT) {
				not_flag = 0;
				matcher++;
			    } else {
				result = 0;
			    }
			}
		}

		key += key_len(key);
	}

	return result;
}