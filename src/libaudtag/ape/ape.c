/*
 * Copyright 2010 John Lindgren
 *
 * This file is part of Audacious.
 *
 * Audacious is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, version 3 of the License.
 *
 * Audacious is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * Audacious. If not, see <http://www.gnu.org/licenses/>.
 *
 * The Audacious team does not consider modular code linking to Audacious or
 * using our public API to be a derived work.
 */

/* TODO:
 * - ReplayGain info
 * - Support updating files that have their tag at the beginning?
 */

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libaudcore/vfs.h>

#include "ape.h"

#define DEBUG(...) fprintf (stderr, "APE: " __VA_ARGS__)

typedef struct
{
    gchar magic[8];
    guint32 version; /* LE */
    guint32 length; /* LE */
    guint32 items; /* LE */
    guint32 flags; /* LE */
    guint64 reserved;
}
APEHeader;

typedef struct
{
    gchar * key, * value;
}
ValuePair;

#define APE_FLAG_HAS_HEADER (1 << 31)
#define APE_FLAG_HAS_NO_FOOTER (1 << 30)
#define APE_FLAG_IS_HEADER (1 << 29)

static gboolean ape_read_header (VFSFile * handle, APEHeader * header)
{
    if (vfs_fread (header, 1, sizeof (APEHeader), handle) != sizeof (APEHeader))
        return FALSE;

    if (strncmp (header->magic, "APETAGEX", 8))
        return FALSE;

    header->version = GUINT32_FROM_LE (header->version);
    header->length = GUINT32_FROM_LE (header->length);
    header->items = GUINT32_FROM_LE (header->items);
    header->flags = GUINT32_FROM_LE (header->flags);

    if (header->length < sizeof (APEHeader))
        return FALSE;

    return TRUE;
}

static gboolean ape_find_header (VFSFile * handle, APEHeader * header, gint *
 start, gint * length, gint * data_start, gint * data_length)
{
    APEHeader secondary;

    if (vfs_fseek (handle, 0, SEEK_SET))
        return FALSE;

    if (ape_read_header (handle, header))
    {
        DEBUG ("Found header at 0, length = %d, version = %d.\n", (gint)
         header->length, (gint) header->version);
        * start = 0;
        * length = header->length;
        * data_start = sizeof (APEHeader);
        * data_length = header->length - sizeof (APEHeader);

        if (! (header->flags & APE_FLAG_HAS_HEADER) || ! (header->flags &
         APE_FLAG_IS_HEADER))
        {
            DEBUG ("Invalid header flags (%u).\n", (guint) header->flags);
            return FALSE;
        }

        if (! (header->flags & APE_FLAG_HAS_NO_FOOTER))
        {
            if (vfs_fseek (handle, header->length, SEEK_CUR))
                return FALSE;

            if (! ape_read_header (handle, & secondary))
            {
                DEBUG ("Expected footer, but found none.\n");
                return FALSE;
            }

            * length += sizeof (APEHeader);
        }

        return TRUE;
    }

    if (vfs_fseek (handle, -sizeof (APEHeader), SEEK_END))
        return FALSE;

    if (ape_read_header (handle, header))
    {
        DEBUG ("Found footer at %d, length = %d, version = %d.\n", (gint)
         vfs_ftell (handle) - (gint) sizeof (APEHeader), (gint) header->length,
         (gint) header->version);
        * start = vfs_ftell (handle) - header->length;
        * length = header->length;
        * data_start = vfs_ftell (handle) - header->length;
        * data_length = header->length - sizeof (APEHeader);

        if ((header->flags & APE_FLAG_HAS_NO_FOOTER) || (header->flags &
         APE_FLAG_IS_HEADER))
        {
            DEBUG ("Invalid footer flags (%u).\n", (guint) header->flags);
            return FALSE;
        }

        if (header->flags & APE_FLAG_HAS_HEADER)
        {
            if (vfs_fseek (handle, -(gint) header->length - sizeof (APEHeader),
             SEEK_CUR))
                return FALSE;

            if (! ape_read_header (handle, & secondary))
            {
                DEBUG ("Expected header, but found none.\n");
                return FALSE;
            }

            * start -= sizeof (APEHeader);
            * length += sizeof (APEHeader);
        }

        return TRUE;
    }

    DEBUG ("No header found.\n");
    return FALSE;
}

static gboolean ape_is_our_file (VFSFile * handle)
{
    APEHeader header;
    gint start, length, data_start, data_length;

    return ape_find_header (handle, & header, & start, & length, & data_start,
     & data_length);
}

static ValuePair * ape_read_item (void * * data, gint length)
{
    guint32 * header = * data;
    gchar * value;
    ValuePair * pair;

    if (length < 8)
    {
        DEBUG ("Expected item, but only %d bytes remain in tag.\n", length);
        return NULL;
    }

    value = memchr ((gchar *) (* data) + 8, 0, length - 8);

    if (value == NULL)
    {
        DEBUG ("Unterminated item key (max length = %d).\n", length - 8);
        return NULL;
    }

    value ++;

    if (header[0] > (gchar *) (* data) + length - value)
    {
        DEBUG ("Item value of length %d, but only %d bytes remain in tag.\n",
         (gint) header[0], (gint) ((gchar *) (* data) + length - value));
        return NULL;
    }

    pair = g_malloc (sizeof (ValuePair));
    pair->key = g_strdup ((gchar *) (* data) + 8);
    pair->value = g_strndup (value, header[0]);

    * data = value + header[0];

    return pair;
}

static GList * ape_read_tag (VFSFile * handle)
{
    GList * list = NULL;
    APEHeader header;
    gint start, length, data_start, data_length;
    void * data, * item;

    if (! ape_find_header (handle, & header, & start, & length, & data_start,
     & data_length))
        return NULL;

    if (vfs_fseek (handle, data_start, SEEK_SET))
        return NULL;

    data = g_malloc (data_length);

    if (vfs_fread (data, 1, data_length, handle) != data_length)
    {
        g_free (data);
        return NULL;
    }

    DEBUG ("Reading %d items:\n", header.items);
    item = data;

    while (header.items --)
    {
        ValuePair * pair = ape_read_item (& item, (gchar *) data + data_length -
         (gchar *) item);

        if (pair == NULL)
            break;

        DEBUG ("Read: %s = %s.\n", pair->key, pair->value);
        list = g_list_prepend (list, pair);
    }

    g_free (data);
    return g_list_reverse (list);
}

static void free_tag_list (GList * list)
{
    while (list != NULL)
    {
        g_free (((ValuePair *) list->data)->key);
        g_free (((ValuePair *) list->data)->value);
        g_free (list->data);
        list = g_list_delete_link (list, list);
    }
}

static Tuple * ape_fill_tuple (Tuple * tuple, VFSFile * handle)
{
    GList * list = ape_read_tag (handle), * node;

    for (node = list; node != NULL; node = node->next)
    {
        gchar * key = ((ValuePair *) node->data)->key;
        gchar * value = ((ValuePair *) node->data)->value;

        if (! strcmp (key, "Artist"))
            tuple_associate_string (tuple, FIELD_ARTIST, NULL, value);
        else if (! strcmp (key, "Title"))
            tuple_associate_string (tuple, FIELD_TITLE, NULL, value);
        else if (! strcmp (key, "Album"))
            tuple_associate_string (tuple, FIELD_ALBUM, NULL, value);
        else if (! strcmp (key, "Comment"))
            tuple_associate_string (tuple, FIELD_COMMENT, NULL, value);
        else if (! strcmp (key, "Genre"))
            tuple_associate_string (tuple, FIELD_GENRE, NULL, value);
        else if (! strcmp (key, "Track"))
            tuple_associate_int (tuple, FIELD_TRACK_NUMBER, NULL, atoi (value));
        else if (! strcmp (key, "Date"))
            tuple_associate_int (tuple, FIELD_YEAR, NULL, atoi (value));
    }

    free_tag_list (list);
    return tuple;
}

static gboolean ape_write_item (VFSFile * handle, const gchar * key,
 const gchar * value, int * written_length)
{
    gint key_len = strlen (key) + 1;
    gint value_len = strlen (value);
    guint32 header[2];

    DEBUG ("Write: %s = %s.\n", key, value);

    header[0] = GUINT32_TO_LE (value_len);
    header[1] = 0;

    if (vfs_fwrite (header, 1, 8, handle) != 8)
        return FALSE;

    if (vfs_fwrite (key, 1, key_len, handle) != key_len)
        return FALSE;

    if (vfs_fwrite (value, 1, value_len, handle) != value_len)
        return FALSE;

    * written_length += 8 + key_len + value_len;
    return TRUE;
}

static gboolean write_string_item (Tuple * tuple, int field, VFSFile * handle,
 const gchar * key, int * written_length, int * written_items)
{
    const gchar * value = tuple_get_string (tuple, field, NULL);

    if (value == NULL)
        return TRUE;

    if (! ape_write_item (handle, key, value, written_length))
        return FALSE;

    (* written_items) ++;
    return TRUE;
}

static gboolean write_integer_item (Tuple * tuple, int field, VFSFile * handle,
 const gchar * key, int * written_length, int * written_items)
{
    gint value = tuple_get_int (tuple, field, NULL);
    gchar scratch[32];

    if (! value)
        return TRUE;

    snprintf (scratch, sizeof scratch, "%d", value);

    if (! ape_write_item (handle, key, scratch, written_length))
        return FALSE;

    (* written_items) ++;
    return TRUE;
}

static gboolean write_header (gint data_length, gint items, gboolean is_header,
 VFSFile * handle)
{
    APEHeader header;

    memcpy (header.magic, "APETAGEX", 8);
    header.version = GUINT32_TO_LE (2000);
    header.length = GUINT32_TO_LE (data_length + sizeof (APEHeader));
    header.items = GUINT32_TO_LE (items);
    header.flags = is_header ? GUINT32_TO_LE (APE_FLAG_HAS_HEADER |
     APE_FLAG_IS_HEADER) : GUINT32_TO_LE (APE_FLAG_HAS_HEADER);
    header.reserved = 0;

    return vfs_fwrite (& header, 1, sizeof (APEHeader), handle) == sizeof
     (APEHeader);
}

static gboolean ape_write_tag (Tuple * tuple, VFSFile * handle)
{
    GList * list = ape_read_tag (handle), * node;
    APEHeader header;
    gint start, length, data_start, data_length, items;

    if (! ape_find_header (handle, & header, & start, & length, & data_start,
     & data_length))
        goto ERROR;

    if (start + length != vfs_fsize (handle))
    {
        DEBUG ("Writing tags is only supported at end of file.\n");
        goto ERROR;
    }

    if (vfs_truncate (handle, start) || vfs_fseek (handle, start, SEEK_SET) ||
     ! write_header (0, 0, TRUE, handle))
        goto ERROR;

    length = 0;
    items = 0;

    if (! write_string_item (tuple, FIELD_ARTIST, handle, "Artist", & length,
     & items) || ! write_string_item (tuple, FIELD_TITLE, handle, "Title",
     & length, & items) || ! write_string_item (tuple, FIELD_ALBUM, handle,
     "Album", & length, & items) || ! write_string_item (tuple, FIELD_COMMENT,
     handle, "Comment", & length, & items) || ! write_string_item (tuple,
     FIELD_GENRE, handle, "Genre", & length, & items) || ! write_integer_item
     (tuple, FIELD_TRACK_NUMBER, handle, "Track", & length, & items) ||
     ! write_integer_item (tuple, FIELD_YEAR, handle, "Date", & length, & items))
        goto ERROR;

    for (node = list; node != NULL; node = node->next)
    {
        gchar * key = ((ValuePair *) node->data)->key;
        gchar * value = ((ValuePair *) node->data)->value;

        if (! strcmp (key, "Artist") || ! strcmp (key, "Title") || ! strcmp
         (key, "Album") || ! strcmp (key, "Comment") || ! strcmp (key, "Genre")
         || ! strcmp (key, "Track") || ! strcmp (key, "Date"))
            continue;

        if (! ape_write_item (handle, key, value, & length))
            goto ERROR;

        items ++;
    }

    DEBUG ("Wrote %d items, %d bytes.\n", items, length);

    if (write_header (length, items, FALSE, handle) || vfs_fseek (handle, start,
     SEEK_SET) || ! write_header (length, items, TRUE, handle))
        goto ERROR;

    free_tag_list (list);
    return TRUE;

ERROR:
    free_tag_list (list);
    return FALSE;
}

tag_module_t ape =
{
    .name = "APE",
    .can_handle_file = ape_is_our_file,
    .populate_tuple_from_file = ape_fill_tuple,
    .write_tuple_to_file = ape_write_tag,
};
