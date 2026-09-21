#include "sms_pdu.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* 3GPP TS 23.038 默认字母表 */
static const char *const s_gsm7[128] = {
    "@", "£", "$", "¥", "è", "é", "ù", "ì", "ò", "Ç", "\n", "Ø", "ø", "\r", "Å", "å",
    "Δ", "_", "Φ", "Γ", "Λ", "Ω", "Π", "Ψ", "Σ", "Θ", "Ξ", NULL, "Æ", "æ", "ß", "É",
    " ", "!", "\"", "#", "¤", "%", "&", "'", "(", ")", "*", "+", ",", "-", ".", "/",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ":", ";", "<", "=", ">", "?",
    "¡", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O",
    "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "Ä", "Ö", "Ñ", "Ü", "§",
    "¿", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o",
    "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "ä", "ö", "ñ", "ü", "à"
};

typedef struct
{
    const char *hex;
    size_t      hex_len; 
    size_t      pos;  
} pdu_reader_t;

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    return -1;
}

static bool read_byte(pdu_reader_t *r, uint8_t *out)
{
    int hi;
    int lo;

    if (r->pos + 2 > r->hex_len)
    {
        return false;
    }

    hi = hex_value(r->hex[r->pos]);
    lo = hex_value(r->hex[r->pos + 1]);
    if (hi < 0 || lo < 0)
    {
        return false;
    }

    *out = (uint8_t)((hi << 4) | lo);
    r->pos += 2;
    return true;
}

static bool read_bytes(pdu_reader_t *r, uint8_t *out, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
    {
        if (!read_byte(r, &out[i]))
        {
            return false;
        }
    }
    return true;
}

static bool skip_bytes(pdu_reader_t *r, size_t len)
{
    if (r->pos + len * 2 > r->hex_len)
    {
        return false;
    }
    r->pos += len * 2;
    return true;
}

static size_t remaining_bytes(const pdu_reader_t *r)
{
    return (r->hex_len - r->pos) / 2;
}

static void append_bytes(char *out, size_t out_size, size_t *oi,
                         const char *data, size_t len)
{
    size_t space;

    if (out_size == 0 || *oi >= out_size - 1)
    {
        return;
    }

    space = out_size - 1 - *oi;
    if (len > space)
    {
        len = space;
    }

    memcpy(out + *oi, data, len);
    *oi += len;
    out[*oi] = '\0';
}

static void append_str(char *out, size_t out_size, size_t *oi, const char *s)
{
    append_bytes(out, out_size, oi, s, strlen(s));
}

static void append_codepoint(char *out, size_t out_size, size_t *oi, uint32_t cp)
{
    char   utf8[4];
    size_t n = 0;

    if (cp <= 0x7F)
    {
        utf8[n++] = (char)cp;
    }
    else if (cp <= 0x7FF)
    {
        utf8[n++] = (char)(0xC0 | (cp >> 6));
        utf8[n++] = (char)(0x80 | (cp & 0x3F));
    }
    else if (cp <= 0xFFFF)
    {
        utf8[n++] = (char)(0xE0 | (cp >> 12));
        utf8[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        utf8[n++] = (char)(0x80 | (cp & 0x3F));
    }
    else
    {
        utf8[n++] = (char)(0xF0 | (cp >> 18));
        utf8[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        utf8[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        utf8[n++] = (char)(0x80 | (cp & 0x3F));
    }

    append_bytes(out, out_size, oi, utf8, n);
}

static bool decode_address(pdu_reader_t *r, char *out, size_t out_size)
{
    uint8_t len;
    uint8_t toa;
    uint8_t addr[10];
    size_t  bytes;
    size_t  oi = 0;
    size_t  i;

    if (!read_byte(r, &len) || !read_byte(r, &toa))
    {
        return false;
    }

    if (len > 20)
    {
        return false;
    }

    bytes = ((size_t)len + 1) / 2;
    if (bytes > sizeof(addr) || !read_bytes(r, addr, bytes))
    {
        return false;
    }

    out[0] = '\0';
    if (toa & 0x80)
    {
        out[oi++] = '+';
        out[oi] = '\0';
    }

    for (i = 0; i < len && oi < out_size - 1; i++)
    {
        uint8_t value = (i % 2 == 0) ? (addr[i / 2] & 0x0F) : (addr[i / 2] >> 4);

        if (value == 0x0F)
        {
            break;
        }
        if (value > 9)
        {
            return false;
        }

        out[oi++] = (char)('0' + value);
    }
    out[oi] = '\0';
    return true;
}

static int bcd_digit(uint8_t value, bool high)
{
    int digit = high ? (value >> 4) : (value & 0x0F);

    return (digit <= 9) ? digit : 0;
}

static bool decode_scts(const uint8_t *scts, char *out, size_t out_size)
{
    int year  = bcd_digit(scts[0], false) * 10 + bcd_digit(scts[0], true);
    int month = bcd_digit(scts[1], false) * 10 + bcd_digit(scts[1], true);
    int day   = bcd_digit(scts[2], false) * 10 + bcd_digit(scts[2], true);
    int hour  = bcd_digit(scts[3], false) * 10 + bcd_digit(scts[3], true);
    int min   = bcd_digit(scts[4], false) * 10 + bcd_digit(scts[4], true);
    int sec   = bcd_digit(scts[5], false) * 10 + bcd_digit(scts[5], true);
    int tz    = bcd_digit(scts[6] & 0x7F, false) * 10 + bcd_digit(scts[6] & 0x7F, true);
    int sign  = (scts[6] & 0x80) ? -1 : 1;

    if (out_size == 0)
    {
        return false;
    }

    snprintf(out, out_size, "%02d/%02d/%02d,%02d:%02d:%02d%+03d",
             year, month, day, hour, min, sec, sign * tz);
    return true;
}

static int dcs_encoding(uint8_t dcs)
{
    switch (dcs & 0x0C)
    {
        case 0x04:
            return 1; /* 8-bit */
        case 0x08:
            return 2; /* UCS2 */
        default:
            return 0; /* GSM 7-bit */
    }
}

static void parse_udh(const uint8_t *udh, size_t header_octets,
                      int *ref_number, int *part_number, int *total_parts)
{
    size_t i = 1; /* 跳过 UDHL */

    while (i + 1 < header_octets)
    {
        uint8_t iei = udh[i++];
        uint8_t iel = udh[i++];

        if (i + iel > header_octets)
        {
            break;
        }

        if (iei == 0x00 && iel == 3)
        {
            *ref_number   = udh[i];
            *total_parts  = udh[i + 1];
            *part_number  = udh[i + 2];
        }
        else if (iei == 0x08 && iel == 4)
        {
            *ref_number   = ((int)udh[i] << 8) | udh[i + 1];
            *total_parts  = udh[i + 2];
            *part_number  = udh[i + 3];
        }

        i += iel;
    }
}

static bool get_septet(const uint8_t *data, size_t data_len, size_t index,
                       uint8_t *out)
{
    size_t byte = (index * 7) / 8;
    int    shift = (int)((index * 7) % 8);
    uint16_t value;

    if (byte >= data_len)
    {
        return false;
    }
    if (shift > 1 && byte + 1 >= data_len)
    {
        return false;
    }

    value = data[byte];
    if (byte + 1 < data_len)
    {
        value |= (uint16_t)data[byte + 1] << 8;
    }

    *out = (uint8_t)((value >> shift) & 0x7F);
    return true;
}

static const char *gsm7_extension(uint8_t code)
{
    switch (code)
    {
        case 0x0A: return "\f";
        case 0x14: return "^";
        case 0x28: return "{";
        case 0x29: return "}";
        case 0x2F: return "\\";
        case 0x3C: return "[";
        case 0x3D: return "~";
        case 0x3E: return "]";
        case 0x40: return "|";
        case 0x65: return "€";
        default:   return "?";
    }
}

static void decode_gsm7(const uint8_t *data, size_t data_len, size_t udl,
                        size_t skip_septets, char *out, size_t out_size)
{
    size_t oi = 0;
    size_t i;

    out[0] = '\0';
    for (i = skip_septets; i < udl; i++)
    {
        uint8_t code;

        if (!get_septet(data, data_len, i, &code))
        {
            break;
        }

        if (code == 0x1B && i + 1 < udl)
        {
            uint8_t ext;

            if (!get_septet(data, data_len, ++i, &ext))
            {
                break;
            }
            append_str(out, out_size, &oi, gsm7_extension(ext));
        }
        else if (code < 128 && s_gsm7[code] != NULL)
        {
            append_str(out, out_size, &oi, s_gsm7[code]);
        }
        else
        {
            append_codepoint(out, out_size, &oi, '?');
        }
    }
}

static void decode_ucs2(const uint8_t *data, size_t len,
                        char *out, size_t out_size)
{
    size_t i = 0;
    size_t oi = 0;

    out[0] = '\0';
    while (i + 1 < len)
    {
        uint16_t u = ((uint16_t)data[i] << 8) | data[i + 1];

        i += 2;
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < len)
        {
            uint16_t lo = ((uint16_t)data[i] << 8) | data[i + 1];

            if (lo >= 0xDC00 && lo <= 0xDFFF)
            {
                uint32_t cp = 0x10000u + (((uint32_t)(u - 0xD800) << 10) |
                                          (uint32_t)(lo - 0xDC00));

                i += 2;
                append_codepoint(out, out_size, &oi, cp);
                continue;
            }
        }

        append_codepoint(out, out_size, &oi, u);
    }
}

static void decode_8bit(const uint8_t *data, size_t len,
                        char *out, size_t out_size)
{
    size_t i;
    size_t oi = 0;

    out[0] = '\0';
    for (i = 0; i < len; i++)
    {
        append_codepoint(out, out_size, &oi, data[i]);
    }
}

bool sms_pdu_extract(const char *resp, const char *tag, char *out, size_t out_size)
{
    const char *p;
    size_t      n = 0;

    if (resp == NULL || tag == NULL || out == NULL || out_size == 0)
    {
        return false;
    }

    p = strstr(resp, tag);
    if (p == NULL)
    {
        return false;
    }

    p = strchr(p, '\n');
    if (p == NULL)
    {
        return false;
    }
    p++;

    while (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t')
    {
        p++;
    }

    while (p[n] != '\0' && n + 1 < out_size && hex_value(p[n]) >= 0)
    {
        out[n] = p[n];
        n++;
    }
    out[n] = '\0';

    return (n > 0 && (n % 2) == 0);
}

bool sms_pdu_decode(const char *pdu_hex, sms_item_t *item,
                    int *ref_number, int *part_number, int *total_parts)
{
    pdu_reader_t r;
    uint8_t      sca_len;
    uint8_t      first_octet;
    uint8_t      dcs;
    uint8_t      scts[7];
    uint8_t      udl;
    uint8_t      ud[256];
    size_t       ud_octets;
    int          encoding;
    int          ref = 0;
    int          part = 1;
    int          total = 1;
    bool         udhi;

    if (pdu_hex == NULL || item == NULL)
    {
        return false;
    }

    memset(item, 0, sizeof(*item));
    if (ref_number != NULL)
    {
        *ref_number = 0;
    }
    if (part_number != NULL)
    {
        *part_number = 1;
    }
    if (total_parts != NULL)
    {
        *total_parts = 1;
    }

    r.hex     = pdu_hex;
    r.hex_len = strlen(pdu_hex);
    r.pos     = 0;
    if (r.hex_len == 0 || (r.hex_len % 2) != 0)
    {
        return false;
    }

    /* SMSC 地址，当前解析不使用 */
    if (!read_byte(&r, &sca_len) || sca_len > 12 || !skip_bytes(&r, sca_len))
    {
        return false;
    }

    if (!read_byte(&r, &first_octet))
    {
        return false;
    }
    if ((first_octet & 0x03) != 0x00) /* TP-MTI = SMS-DELIVER */
    {
        return false;
    }
    udhi = (first_octet & 0x40) != 0;

    if (!decode_address(&r, item->sender, sizeof(item->sender)))
    {
        return false;
    }

    /* TP-PID */
    if (!skip_bytes(&r, 1))
    {
        return false;
    }
    if (!read_byte(&r, &dcs))
    {
        return false;
    }
    if (!read_bytes(&r, scts, sizeof(scts)))
    {
        return false;
    }
    if (!decode_scts(scts, item->timestamp, sizeof(item->timestamp)))
    {
        return false;
    }
    if (!read_byte(&r, &udl))
    {
        return false;
    }

    encoding = dcs_encoding(dcs);
    if (encoding == 0)
    {
        ud_octets = ((size_t)udl * 7 + 7) / 8;
    }
    else
    {
        ud_octets = udl;
    }

    if (ud_octets > sizeof(ud) || ud_octets > remaining_bytes(&r))
    {
        return false;
    }
    if (!read_bytes(&r, ud, ud_octets))
    {
        return false;
    }

    if (udhi)
    {
        size_t header_octets;

        if (ud_octets == 0)
        {
            return false;
        }

        header_octets = (size_t)ud[0] + 1;
        if (header_octets > ud_octets)
        {
            return false;
        }

        parse_udh(ud, header_octets, &ref, &part, &total);

        if (encoding == 0)
        {
            size_t header_septets = (header_octets * 8 + 6) / 7;

            if (header_septets > udl)
            {
                return false;
            }
            decode_gsm7(ud, ud_octets, udl, header_septets,
                        item->text, sizeof(item->text));
        }
        else
        {
            size_t data_len;

            if (header_octets > udl)
            {
                return false;
            }
            data_len = (size_t)udl - header_octets;
            if (encoding == 2)
            {
                decode_ucs2(ud + header_octets, data_len,
                            item->text, sizeof(item->text));
            }
            else
            {
                decode_8bit(ud + header_octets, data_len,
                            item->text, sizeof(item->text));
            }
        }
    }
    else if (encoding == 0)
    {
        decode_gsm7(ud, ud_octets, udl, 0, item->text, sizeof(item->text));
    }
    else if (encoding == 2)
    {
        decode_ucs2(ud, ud_octets, item->text, sizeof(item->text));
    }
    else
    {
        decode_8bit(ud, ud_octets, item->text, sizeof(item->text));
    }

    if (ref_number != NULL)
    {
        *ref_number = ref;
    }
    if (part_number != NULL)
    {
        *part_number = part;
    }
    if (total_parts != NULL)
    {
        *total_parts = total;
    }

    return true;
}
