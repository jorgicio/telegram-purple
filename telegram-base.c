/*
    This file is part of telegram-purple

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA

    Copyright Matthias Jentsch, Vitaly Valtman, Christopher Althaus, Markus Endres 2014
*/
#define _GNU_SOURCE
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <assert.h>

#include <tgl.h>
#include <tgl-binlog.h>

#include <glib.h>
#include <request.h>
#include <openssl/sha.h>

#include "telegram-purple.h"
#include "msglog.h"
#include "tgp-2prpl.h"
#include "tgp-structs.h"
#include "lodepng/lodepng.h"


#define DC_SERIALIZED_MAGIC 0x868aa81d
#define STATE_FILE_MAGIC 0x28949a93
#define SECRET_CHAT_FILE_MAGIC 0x37a1988a


void read_state_file (struct tgl_state *TLS) {
  char *name = 0;
  if (asprintf (&name, "%s/%s", TLS->base_path, "state") < 0) {
    return;
  }

  int state_file_fd = open (name, O_CREAT | O_RDWR, 0600);
  free (name);

  if (state_file_fd < 0) {
    return;
  }
  int version, magic;
  if (read (state_file_fd, &magic, 4) < 4) { close (state_file_fd); return; }
  if (magic != (int)STATE_FILE_MAGIC) { close (state_file_fd); return; }
  if (read (state_file_fd, &version, 4) < 4 || version < 0) { close (state_file_fd); return; }
  int x[4];
  if (read (state_file_fd, x, 16) < 16) {
    close (state_file_fd); 
    return;
  }
  int pts = x[0];
  int qts = x[1];
  int seq = x[2];
  int date = x[3];
  close (state_file_fd); 
  bl_do_set_seq (TLS, seq);
  bl_do_set_pts (TLS, pts);
  bl_do_set_qts (TLS, qts);
  bl_do_set_date (TLS, date);
}

void write_state_file (struct tgl_state *TLS) {
  int wseq;
  int wpts;
  int wqts;
  int wdate;
  wseq = TLS->seq; wpts = TLS->pts; wqts = TLS->qts; wdate = TLS->date;
  
  char *name = 0;
  if (asprintf (&name, "%s/%s", TLS->base_path, "state") < 0) {
    return;
  }

  int state_file_fd = open (name, O_CREAT | O_RDWR, 0600);
  free (name);

  if (state_file_fd < 0) {
    return;
  }
  int x[6];
  x[0] = STATE_FILE_MAGIC;
  x[1] = 0;
  x[2] = wpts;
  x[3] = wqts;
  x[4] = wseq;
  x[5] = wdate;
  assert (write (state_file_fd, x, 24) == 24);
  close (state_file_fd); 
}

void write_dc (struct tgl_dc *DC, void *extra) {
  int auth_file_fd = *(int *)extra;
  if (!DC) { 
    int x = 0;
    assert (write (auth_file_fd, &x, 4) == 4);
    return;
  } else {
    int x = 1;
    assert (write (auth_file_fd, &x, 4) == 4);
  }

  assert (DC->has_auth);

  assert (write (auth_file_fd, &DC->port, 4) == 4);
  int l = strlen (DC->ip);
  assert (write (auth_file_fd, &l, 4) == 4);
  assert (write (auth_file_fd, DC->ip, l) == l);
  assert (write (auth_file_fd, &DC->auth_key_id, 8) == 8);
  assert (write (auth_file_fd, DC->auth_key, 256) == 256);
}

void write_auth_file (struct tgl_state *TLS) {
  char *name = 0;
  if (asprintf (&name, "%s/%s", TLS->base_path, "auth") < 0) {
    return;
  }
  int auth_file_fd = open (name, O_CREAT | O_RDWR, 0600);
  free (name);
  if (auth_file_fd < 0) { return; }
  int x = DC_SERIALIZED_MAGIC;
  assert (write (auth_file_fd, &x, 4) == 4);
  assert (write (auth_file_fd, &TLS->max_dc_num, 4) == 4);
  assert (write (auth_file_fd, &TLS->dc_working_num, 4) == 4);

  tgl_dc_iterator_ex (TLS, write_dc, &auth_file_fd);

  assert (write (auth_file_fd, &TLS->our_id, 4) == 4);
  close (auth_file_fd);
}

void read_dc (struct tgl_state *TLS, int auth_file_fd, int id, unsigned ver) {
  int port = 0;
  assert (read (auth_file_fd, &port, 4) == 4);
  int l = 0;
  assert (read (auth_file_fd, &l, 4) == 4);
  assert (l >= 0 && l < 100);
  char ip[100];
  assert (read (auth_file_fd, ip, l) == l);
  ip[l] = 0;

  long long auth_key_id;
  static unsigned char auth_key[256];
  assert (read (auth_file_fd, &auth_key_id, 8) == 8);
  assert (read (auth_file_fd, auth_key, 256) == 256);

  //bl_do_add_dc (id, ip, l, port, auth_key_id, auth_key);
  bl_do_dc_option (TLS, id, 2, "DC", l, ip, port);
  bl_do_set_auth_key_id (TLS, id, auth_key);
  bl_do_dc_signed (TLS, id);
}

int error_if_val_false (struct tgl_state *TLS, int val, const char *msg) {
  if (!val) {
    connection_data *conn = TLS->ev_base;
    purple_connection_error_reason (conn->gc, PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED, msg);
    return 1;
  }
  return 0;
}

void empty_auth_file (struct tgl_state *TLS) {
  if (TLS->test_mode) {
    bl_do_dc_option (TLS, 1, 0, "", strlen (TG_SERVER_TEST_1), TG_SERVER_TEST_1, 443);
    bl_do_dc_option (TLS, 2, 0, "", strlen (TG_SERVER_TEST_2), TG_SERVER_TEST_2, 443);
    bl_do_dc_option (TLS, 3, 0, "", strlen (TG_SERVER_TEST_3), TG_SERVER_TEST_3, 443);
    bl_do_set_working_dc (TLS, TG_SERVER_TEST_DEFAULT);
  } else {
    bl_do_dc_option (TLS, 1, 0, "", strlen (TG_SERVER_1), TG_SERVER_1, 443);
    bl_do_dc_option (TLS, 2, 0, "", strlen (TG_SERVER_2), TG_SERVER_2, 443);
    bl_do_dc_option (TLS, 3, 0, "", strlen (TG_SERVER_3), TG_SERVER_3, 443);
    bl_do_dc_option (TLS, 4, 0, "", strlen (TG_SERVER_4), TG_SERVER_4, 443);
    bl_do_dc_option (TLS, 5, 0, "", strlen (TG_SERVER_5), TG_SERVER_5, 443);
    bl_do_set_working_dc (TLS, TG_SERVER_DEFAULT);;
  }
}

void read_auth_file (struct tgl_state *TLS) {
  char *name = 0;
  if (asprintf (&name, "%s/%s", TLS->base_path, "auth") < 0) {
    return;
  }
  int auth_file_fd = open (name, O_CREAT | O_RDWR, 0600);
  free (name);
  if (auth_file_fd < 0) {
    empty_auth_file (TLS);
    return;
  }
  assert (auth_file_fd >= 0);
  unsigned x;
  unsigned m;
  if (read (auth_file_fd, &m, 4) < 4 || (m != DC_SERIALIZED_MAGIC)) {
    close (auth_file_fd);
    empty_auth_file (TLS);
    return;
  }
  assert (read (auth_file_fd, &x, 4) == 4);
  assert (x > 0);
  int dc_working_num;
  assert (read (auth_file_fd, &dc_working_num, 4) == 4);
  
  int i;
  for (i = 0; i <= (int)x; i++) {
    int y;
    assert (read (auth_file_fd, &y, 4) == 4);
    if (y) {
      read_dc (TLS, auth_file_fd, i, m);
    }
  }
  bl_do_set_working_dc (TLS, dc_working_num);
  int our_id;
  int l = read (auth_file_fd, &our_id, 4);
  if (l < 4) {
    assert (!l);
  }
  if (our_id) {
    bl_do_set_our_id (TLS, our_id);
  }
  close (auth_file_fd);
}


void write_secret_chat (tgl_peer_t *_P, void *extra) {
  struct tgl_secret_chat *P = (void *)_P;
  if (tgl_get_peer_type (P->id) != TGL_PEER_ENCR_CHAT) { return; }
  if (P->state != sc_ok) { return; }
  int *a = extra;
  int fd = a[0];
  a[1] ++;
  
  int id = tgl_get_peer_id (P->id);
  assert (write (fd, &id, 4) == 4);
  //assert (write (fd, &P->flags, 4) == 4);
  int l = strlen (P->print_name);
  assert (write (fd, &l, 4) == 4);
  assert (write (fd, P->print_name, l) == l);
  assert (write (fd, &P->user_id, 4) == 4);
  assert (write (fd, &P->admin_id, 4) == 4);
  assert (write (fd, &P->date, 4) == 4);
  assert (write (fd, &P->ttl, 4) == 4);
  assert (write (fd, &P->layer, 4) == 4);
  assert (write (fd, &P->access_hash, 8) == 8);
  assert (write (fd, &P->state, 4) == 4);
  assert (write (fd, &P->key_fingerprint, 8) == 8);
  assert (write (fd, &P->key, 256) == 256);
  assert (write (fd, &P->first_key_sha, 20) == 20);
  assert (write (fd, &P->in_seq_no, 4) == 4);
  assert (write (fd, &P->last_in_seq_no, 4) == 4);
  assert (write (fd, &P->out_seq_no, 4) == 4);
}

void write_secret_chat_file (struct tgl_state *TLS) {
  char *name = 0;
  if (asprintf (&name, "%s/%s", TLS->base_path, "secret") < 0) {
    return;
  }
  int secret_chat_fd = open (name, O_CREAT | O_RDWR, 0600);
  free (name);
  assert (secret_chat_fd >= 0);
  int x = SECRET_CHAT_FILE_MAGIC;
  assert (write (secret_chat_fd, &x, 4) == 4);
  x = 2;
  assert (write (secret_chat_fd, &x, 4) == 4); // version
  assert (write (secret_chat_fd, &x, 4) == 4); // num
  
  int y[2];
  y[0] = secret_chat_fd;
  y[1] = 0;
  
  tgl_peer_iterator_ex (TLS, write_secret_chat, y);
  
  lseek (secret_chat_fd, 8, SEEK_SET);
  assert (write (secret_chat_fd, &y[1], 4) == 4);
  close (secret_chat_fd);
}

void read_secret_chat (struct tgl_state *TLS, int fd, int v) {
  int id, l, user_id, admin_id, date, ttl, layer, state;
  long long access_hash, key_fingerprint;
  static char s[1000];
  static unsigned char key[256];
  static unsigned char sha[20];
  assert (read (fd, &id, 4) == 4);
  //assert (read (fd, &flags, 4) == 4);
  assert (read (fd, &l, 4) == 4);
  assert (l > 0 && l < 1000);
  assert (read (fd, s, l) == l);
  assert (read (fd, &user_id, 4) == 4);
  assert (read (fd, &admin_id, 4) == 4);
  assert (read (fd, &date, 4) == 4);
  assert (read (fd, &ttl, 4) == 4);
  assert (read (fd, &layer, 4) == 4);
  assert (read (fd, &access_hash, 8) == 8);
  assert (read (fd, &state, 4) == 4);
  assert (read (fd, &key_fingerprint, 8) == 8);
  assert (read (fd, &key, 256) == 256);
  if (v >= 2) {
    assert (read (fd, sha, 20) == 20);
  }
  int in_seq_no = 0, out_seq_no = 0, last_in_seq_no = 0;
  if (v >= 1) {
    assert (read (fd, &in_seq_no, 4) == 4);
    assert (read (fd, &last_in_seq_no, 4) == 4);
    assert (read (fd, &out_seq_no, 4) == 4);
  }
  
  bl_do_encr_chat_create (TLS, id, user_id, admin_id, s, l);
  struct tgl_secret_chat  *P = (void *)tgl_peer_get (TLS, TGL_MK_ENCR_CHAT (id));
  assert (P && (P->flags & FLAG_CREATED));
  bl_do_encr_chat_set_date (TLS, P, date);
  bl_do_encr_chat_set_ttl (TLS, P, ttl);
  bl_do_encr_chat_set_layer (TLS ,P, layer);
  bl_do_encr_chat_set_state (TLS, P, state);
  bl_do_encr_chat_set_key (TLS, P, key, key_fingerprint);
  if (v >= 2) {
    bl_do_encr_chat_set_sha (TLS, P, sha);
  } else {
    SHA1 ((void *)key, 256, sha);
    bl_do_encr_chat_set_sha (TLS, P, sha);
  }
  if (v >= 1) {
    bl_do_encr_chat_set_seq (TLS, P, in_seq_no, last_in_seq_no, out_seq_no);
  }
  bl_do_encr_chat_set_access_hash (TLS, P, access_hash);
}

void read_secret_chat_file (struct tgl_state *TLS) {
  char *name = 0;
  if (asprintf (&name, "%s/%s", TLS->base_path, "secret") < 0) {
    return;
  }
  
  int secret_chat_fd = open (name, O_RDWR, 0600);
  free (name);
  
  if (secret_chat_fd < 0) { return; }
  
  int x;
  if (read (secret_chat_fd, &x, 4) < 4) { close (secret_chat_fd); return; }
  if (x != SECRET_CHAT_FILE_MAGIC) { close (secret_chat_fd); return; }
  int v = 0;
  assert (read (secret_chat_fd, &v, 4) == 4);
  assert (v == 0 || v == 1 || v == 2); // version
  assert (read (secret_chat_fd, &x, 4) == 4);
  assert (x >= 0);
  while (x -- > 0) {
    read_secret_chat (TLS, secret_chat_fd, v);
  }
  close (secret_chat_fd);
}

void telegram_export_authorization (struct tgl_state *TLS);
void export_auth_callback (struct tgl_state *TLS, void *extra, int success) {
  if (!error_if_val_false(TLS, success, "Authentication Export failed.")) {
    telegram_export_authorization (TLS);
  }
}

void telegram_export_authorization (struct tgl_state *TLS) {
  int i;
  for (i = 0; i <= TLS->max_dc_num; i++) if (TLS->DC_list[i] && !tgl_signed_dc (TLS, TLS->DC_list[i])) {
    tgl_do_export_auth (TLS, i, export_auth_callback, (void*)(long)TLS->DC_list[i]);    
    return;
  }
  write_auth_file (TLS);
  on_ready (TLS);
}

static void request_code (struct tgl_state *TLS);
static void request_name_and_code (struct tgl_state *TLS);
static void code_receive_result (struct tgl_state *TLS, void *extra, int success, struct tgl_user *U) {
  if (!success) {
    debug ("Bad code...\n");
    request_code (TLS);
  } else {
    telegram_export_authorization (TLS);
  }
}

static void code_auth_receive_result (struct tgl_state *TLS, void *extra, int success, struct tgl_user *U) {
  if (!success) {
    debug ("Bad code...\n");
    request_name_and_code (TLS);
  } else {
    telegram_export_authorization (TLS);
  }
}

void request_code_entered (gpointer data, const gchar *code) {
  struct tgl_state *TLS = data;
  connection_data *conn = TLS->ev_base;
  char const *username = purple_account_get_username(conn->pa);
  tgl_do_send_code_result (TLS, username, conn->hash, code, code_receive_result, 0) ;
}

static void request_code_canceled (gpointer data) {
  struct tgl_state *TLS = data;
  connection_data *conn = TLS->ev_base;

  purple_connection_error_reason(conn->gc,
      PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED, "registration canceled");
}

static void request_name_code_entered (PurpleConnection* gc, PurpleRequestFields* fields) {
  connection_data *conn = purple_connection_get_protocol_data(gc);
  struct tgl_state *TLS = conn->TLS;
  char const *username = purple_account_get_username(conn->pa);
  
  const char* first = purple_request_fields_get_string(fields, "first_name");
  const char* last = purple_request_fields_get_string(fields, "last_name");
  const char* code = purple_request_fields_get_string(fields, "code");
  if (!first || !last || !code) {
    request_name_and_code (TLS);
    return;
  }
  
  tgl_do_send_code_result_auth (TLS, username, conn->hash, code, first, last, code_auth_receive_result, NULL);
}

static void request_code (struct tgl_state *TLS) {
  debug ("Client is not registered, registering...\n");
  connection_data *conn = TLS->ev_base;
  int compat = purple_account_get_bool (tg_get_acc(TLS), "compat-verification", 0);
  
  if (compat || ! purple_request_input (conn->gc, "Telegram Code", "Enter Telegram Code",
        "Telegram wants to verify your identity, please enter the code, that you have received via SMS.",
        NULL, 0, 0, "code", "OK", G_CALLBACK(request_code_entered), "Cancel",
        G_CALLBACK(request_code_canceled), conn->pa, NULL, NULL, TLS)) {

    // purple request API is not available, so we create a new conversation (the Telegram system
    // account "7770000") to prompt the user for the code
          
    conn->in_fallback_chat = 1;
    purple_connection_set_state (conn->gc, PURPLE_CONNECTED);
    PurpleConversation *conv = purple_conversation_new (PURPLE_CONV_TYPE_IM, conn->pa, "777000");
    purple_conversation_write (conv, "777000", "What is your SMS verification code?",
          PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_SYSTEM, 0);
  }
}

static void request_name_and_code (struct tgl_state *TLS) {
  debug ("Phone is not registered, registering...\n");

  connection_data *conn = TLS->ev_base;

  PurpleRequestFields* fields = purple_request_fields_new();
  PurpleRequestField* field = 0;

  PurpleRequestFieldGroup* group = purple_request_field_group_new("Registration");
  field = purple_request_field_string_new("first_name", "First Name", "", 0);
  purple_request_field_group_add_field(group, field);
  field = purple_request_field_string_new("last_name", "Last Name", "", 0);
  purple_request_field_group_add_field(group, field);
  purple_request_fields_add_group(fields, group);

  group = purple_request_field_group_new("Authorization");
  field = purple_request_field_string_new("code", "Telegram Code", "", 0);
  purple_request_field_group_add_field(group, field);
  purple_request_fields_add_group(fields, group);

  if (!purple_request_fields (conn->gc, "Register", "Please register your phone number.", NULL, fields, "Ok",
    G_CALLBACK( request_name_code_entered ), "Cancel", NULL, conn->pa, NULL, NULL, conn->gc)) {
    // purple_request API not available
    const char *error = "Phone number is not registered, please register your phone on a different client.";
    purple_connection_error_reason (conn->gc, PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED, error);
    purple_notify_error(_telegram_protocol, "Not Registered", "Not Registered", error);
  }
}

static void sign_in_callback (struct tgl_state *TLS, void *extra, int success, int registered, const char *mhash) {
  connection_data *conn = TLS->ev_base;
  if (!error_if_val_false (TLS, success, "Invalid or non-existing phone number.")) {
    conn->hash = strdup (mhash);
    if (registered) {
      request_code (TLS);
    } else {
      request_name_and_code (TLS);
    }
  }
}

static void telegram_send_sms (struct tgl_state *TLS) {
  if (tgl_signed_dc (TLS, TLS->DC_working)) {
    telegram_export_authorization (TLS);
    return;
  }
  connection_data *conn = TLS->ev_base;
  char const *username = purple_account_get_username(conn->pa);
  tgl_do_send_code (TLS, username, sign_in_callback, 0);
}

static int all_authorized (struct tgl_state *TLS) {
  int i;
  for (i = 0; i <= TLS->max_dc_num; i++) if (TLS->DC_list[i]) {
    if (!tgl_authorized_dc (TLS, TLS->DC_list[i])) {
      return 0;
    }
  }
  return 1;
}

static int check_all_authorized (gpointer arg) {
  struct tgl_state *TLS = arg;
  if (all_authorized (TLS)) {
    telegram_send_sms (TLS);    
    return FALSE;
  } else {
    return TRUE;
  }
}
    
void telegram_login (struct tgl_state *TLS) {    
  read_auth_file (TLS);
  read_state_file (TLS);
  read_secret_chat_file (TLS);
  if (all_authorized (TLS)) {
    telegram_send_sms (TLS);
    return;
  }
  purple_timeout_add (100, check_all_authorized, TLS);
}

PurpleConversation *chat_show (PurpleConnection *gc, int id) {
  debug ("show chat");
  connection_data *conn = purple_connection_get_protocol_data(gc);
  
  PurpleConversation *convo = purple_find_chat(gc, id);
  if (! convo) {
    gchar *name = g_strdup_printf ("%d", id);
    if (! g_hash_table_contains (conn->joining_chats, name)) {
      g_hash_table_insert(conn->joining_chats, name, 0);
      tgl_do_get_chat_info (conn->TLS, TGL_MK_CHAT(id), 0, on_chat_get_info, NULL);
    } else {
      g_free(name);
    }
  }
  return convo;
}

int chat_add_message (struct tgl_state *TLS, struct tgl_message *M, char *text) {
  connection_data *conn = TLS->ev_base;
  
  if (chat_show (conn->gc, tgl_get_peer_id (M->to_id))) {
    p2tgl_got_chat_in(TLS, M->to_id, M->from_id, text ? text : M->message,
                      M->service ? PURPLE_MESSAGE_SYSTEM : PURPLE_MESSAGE_RECV, M->date);
    
    pending_reads_add (conn->pending_reads, M->to_id);
    if (p2tgl_status_is_present(purple_account_get_active_status(conn->pa))) {
      pending_reads_send_all (conn->pending_reads, conn->TLS);
    }
    return 1;
  } else {
    // add message once the chat was initialised
    struct message_text *mt = message_text_init (M, text);
    g_queue_push_tail (conn->new_messages, mt);
    return 0;
  }
}

void chat_add_all_users (PurpleConversation *pc, struct tgl_chat *chat) {
  struct tgl_chat_user *curr =  chat->user_list;
  if (!curr) {
    warning ("add_all_users_to_chat: chat contains no user list, cannot add users\n.");
    return;
  }
  
  int i;
  for (i = 0; i < chat->user_list_size; i++) {
    struct tgl_chat_user *uid = (curr + i);
    int flags = (chat->admin_id == uid->user_id ? PURPLE_CBFLAGS_FOUNDER : PURPLE_CBFLAGS_NONE);
    p2tgl_conv_add_user(pc, *uid, NULL, flags, 0);
  }
}

/**
 * This function generates a png image to visualize the sha1 key from an encrypted chat.
 */
int generate_ident_icon (struct tgl_state *TLS, unsigned char* sha1_key)
{
  int colors[4] = {
    0xffffff,
    0xd5e6f3,
    0x2d5775,
    0x2f99c9
  };
  unsigned img_size = 160;
  unsigned char* image = (unsigned char*)malloc (img_size * img_size * 4);
  assert (image);
  unsigned x, y, i, j, idx = 0;
  int bitpointer = 0;
  for (y = 0; y < 8; y++)
  {
    unsigned offset_y = y * img_size * 4 * (img_size / 8);
    for (x = 0; x < 8; x++)
    {
      int offset = bitpointer / 8;
      int shiftOffset = bitpointer % 8;
      int val = sha1_key[offset + 3] << 24 | sha1_key[offset + 2] << 16 | sha1_key[offset + 1] << 8 | sha1_key[offset];
      idx = abs((val >> shiftOffset) & 3) % 4;
      bitpointer += 2;
      unsigned offset_x = x * 4 * (img_size / 8);
      for (i = 0; i < img_size / 8; i++)
      {
        unsigned off_y = offset_y + i * img_size * 4;
        for (j = 0; j < img_size / 8; j++)
        {
          unsigned off_x = offset_x + j * 4;
          image[off_y + off_x + 0] = (colors[idx] >> 16) & 0xFF;
          image[off_y + off_x + 1] = (colors[idx] >> 8) & 0xFF;
          image[off_y + off_x + 2] = colors[idx] & 0xFF;
          image[off_y + off_x + 3] = 0xFF;
        }
      }
    }
  }
  unsigned char* png;
  size_t pngsize;
  unsigned error = lodepng_encode32(&png, &pngsize, image, img_size, img_size);
  int imgStoreId = -1;
  if(!error)
  {
    imgStoreId = purple_imgstore_add_with_id (png, pngsize, NULL);
    used_images_add ((connection_data*)TLS->ev_base, imgStoreId);
  }
  g_free(image);
  return imgStoreId;
}
