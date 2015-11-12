/*
 * event_handler.c
 *
 *  Created on: Nov 8, 2015
 *      Author: pchero
 */

#include "asterisk.h"

#include <stdbool.h>
#include <event2/event.h>
#include <event2/thread.h>
#include <errno.h>

#include "asterisk/json.h"
#include "asterisk/utils.h"
#include "asterisk/cli.h"
#include "asterisk/uuid.h"
#include "asterisk/file.h"

#include "db_handler.h"

#define TEMP_FILENAME "/tmp/asterisk_outbound_tmp.txt"


struct event_base*  g_base = NULL;

static int init_outbound(void);

static void cb_campaign_start(__attribute__((unused)) int fd, __attribute__((unused)) short event, __attribute__((unused)) void *arg);

static struct ast_json* get_campaign_info_for_dialing(void);
static struct ast_json* get_plan_info(const char* uuid);
static struct ast_json* get_dl_master_info(const char* uuid);
static int update_campaign_info_status(const char* uuid, const char* status);

static void dial_desktop(const struct ast_json* j_camp, const struct ast_json* j_plan, const struct ast_json* j_dlma);
static void dial_power(const struct ast_json* j_camp, const struct ast_json* j_plan, const struct ast_json* j_dlma);
static void dial_predictive(struct ast_json* j_camp, struct ast_json* j_plan, struct ast_json* j_dlma);
static void dial_robo(const struct ast_json* j_camp, const struct ast_json* j_plan, const struct ast_json* j_dlma);
static void dial_redirect(const struct ast_json* j_camp, const struct ast_json* j_plan, const struct ast_json* j_dlma);

static struct ast_json* get_dl_dial_available(const struct ast_json* j_dlma, const char* dial_mode);
static struct ast_json* get_dl_available_predictive(const struct ast_json* j_dlma, const struct ast_json* j_plan);

char* get_utc_timestamp(void);
struct ast_json* cmd_queue_show(const char* name);

// todo
static int check_dial_avaiable(const struct ast_json* j_camp, const struct ast_json* j_plan, const struct ast_json* j_dlma);
struct ast_json* create_dialing_info(const struct ast_json* j_camp, const struct ast_json* j_plan, const struct ast_json* j_dlma, const struct ast_json* j_dl_list);
struct ast_json* create_dial_info(const struct ast_json* j_camp);
int cmd_originate(const struct ast_json* j_dial);
int memdb_insert(const char* table, const struct ast_json* j_data);


int run_outbound(void)
{
    int ret;
    struct event* ev;
    struct timeval tm_fast = {3, 20000};    // 20 ms
//    struct timeval tm_slow = {0, 500000};   // 500 ms

    // init libevent
    ret = init_outbound();
    if(ret == false) {
        ast_log(LOG_ERROR, "Could not initiate outbound.\n");
        return false;
    }

    // check start.
    ev = event_new(g_base, -1, EV_TIMEOUT | EV_PERSIST, cb_campaign_start, NULL);
    event_add(ev, &tm_fast);

    event_base_loop(g_base, 0);

    return true;
}

static int init_outbound(void)
{
    int ret;
    struct ast_json* j_res;
    db_res_t* db_res;

    ret = evthread_use_pthreads();
    if(ret == -1){
        ast_log(LOG_ERROR, "Could not initiated event thread.");
    }

    // init libevent
    if(g_base == NULL) {
        ast_log(LOG_DEBUG, "event_base_new\n");
        g_base = event_base_new();
    }

    if(g_base == NULL) {
        ast_log(LOG_ERROR, "Could not initiate libevent. err[%d:%s]\n", errno, strerror(errno));
        return false;
    }

    // check database tables.
    db_res = db_query("select 1 from campaign limit 1;");
    if(db_res == NULL) {
        ast_log(LOG_ERROR, "Could not initiate libevent. Table is not ready.\n");
        return false;
    }
    j_res = db_get_record(db_res);
    db_free(db_res);
    ast_json_unref(j_res);

    ast_log(LOG_NOTICE, "Initiated outbound.\n");

    return true;
}

void stop_outbound(void)
{
    struct timeval sec;

    sec.tv_sec = 1;
    sec.tv_usec = 0;

    event_base_loopexit(g_base, &sec);

    return;
}

/**
 *  @brief  Check start status campaign and trying to make a call.
 */
static void cb_campaign_start(__attribute__((unused)) int fd, __attribute__((unused)) short event, __attribute__((unused)) void *arg)
{
    struct ast_json* j_camp;
    struct ast_json* j_plan;
    struct ast_json* j_dlma;
    const char* dial_mode;

    ast_log(LOG_DEBUG, "cb_campagin start\n");
    cmd_queue_show("sales");

    j_camp = get_campaign_info_for_dialing();
    if(j_camp == NULL) {
        // Nothing.
        return;
    }
    ast_log(LOG_DEBUG, "Get campaign info. camp[%s]\n", ast_json_string_get(ast_json_object_get(j_camp, "uuid")));

    // get plan
    j_plan = get_plan_info(ast_json_string_get(ast_json_object_get(j_camp, "plan")));
    if(j_plan == NULL) {
        ast_log(LOG_WARNING, "Could not get plan info. Stopping campaign camp[%s], plan[%s]\n",
                ast_json_string_get(ast_json_object_get(j_camp, "uuid")),
                ast_json_string_get(ast_json_object_get(j_camp, "plan"))
                );
        update_campaign_info_status(ast_json_string_get(ast_json_object_get(j_camp, "uuid")), "stopping");
        ast_json_unref(j_camp);
        return;
    }

    // get dl_master_info
    j_dlma = get_dl_master_info(ast_json_string_get(ast_json_object_get(j_camp, "dlma")));
    if(j_dlma == NULL)
    {
        ast_log(LOG_ERROR, "Could not find dial list master info. Stopping campaign. camp[%s], dlma[%s]\n",
                ast_json_string_get(ast_json_object_get(j_camp, "uuid")),
                ast_json_string_get(ast_json_object_get(j_camp, "dlma"))
                );
        update_campaign_info_status(ast_json_string_get(ast_json_object_get(j_camp, "uuid")), "stopping");
        ast_json_unref(j_camp);
        ast_json_unref(j_plan);
        return;
    }

    // get dial_mode
    dial_mode = ast_json_string_get(ast_json_object_get(j_plan, "dial_mode"));
    if(dial_mode == NULL) {
        ast_log(LOG_ERROR, "Plan has no dial_mode. Stopping campaign. camp[%s], plan[%s]",
                ast_json_string_get(ast_json_object_get(j_camp, "uuid")),
                ast_json_string_get(ast_json_object_get(j_camp, "plan"))
                );

        update_campaign_info_status(ast_json_string_get(ast_json_object_get(j_camp, "uuid")), "stopping");
        ast_json_unref(j_camp);
        ast_json_unref(j_plan);
        ast_json_unref(j_dlma);
        return;
    }

    if(strcmp(dial_mode, "desktop") == 0)
    {
        dial_desktop(j_camp, j_plan, j_dlma);
    }
    else if(strcmp(dial_mode, "power") == 0)
    {
        dial_power(j_camp, j_plan, j_dlma);
    }
    else if(strcmp(dial_mode, "predictive") == 0)
    {
        dial_predictive(j_camp, j_plan, j_dlma);
    }
    else if(strcmp(dial_mode, "robo") == 0)
    {
        dial_robo(j_camp, j_plan, j_dlma);
    }
    else if(strcmp(dial_mode, "redirect") == 0)
    {
        dial_redirect(j_camp, j_plan, j_dlma);
    }
    else
    {
        ast_log(LOG_ERROR, "No match dial_mode. dial_mode[%s]\n", dial_mode);
    }


    // release
    ast_json_unref(j_camp);
    ast_json_unref(j_plan);
    ast_json_unref(j_dlma);

    return;
}

/**
 * Get campaign for dialing.
 * @return
 */
static struct ast_json* get_campaign_info_for_dialing(void)
{
    struct ast_json* j_res;
    db_res_t* db_res;
    char* sql;

    // get "start" status campaign only.
    ast_asprintf(&sql, "select * from campaign where status = \"%s\" order by rand() limit 1;",
            "start"
            );

    db_res = db_query(sql);
    ast_free(sql);
    if(db_res == NULL) {
        ast_log(LOG_WARNING, "Could not get campaign info.");
        return NULL;
    }

    j_res = db_get_record(db_res);
    db_free(db_res);

    return j_res;
}

/**
 * Get plan record info.
 * @param uuid
 * @return
 */
static struct ast_json* get_plan_info(const char* uuid)
{
    char* sql;
    struct ast_json* j_res;
    db_res_t* db_res;

    if(uuid == NULL) {
        ast_log(LOG_WARNING, "Invalid input parameters.\n");
        return NULL;
    }

    ast_asprintf(&sql, "select * from plan where uuid = \"%s\";", uuid);

    db_res = db_query(sql);
    ast_free(sql);
    if(db_res == NULL) {
        ast_log(LOG_ERROR, "Could not get plan info. uuid[%s]\n", uuid);
        return NULL;
    }

    j_res = db_get_record(db_res);
    db_free(db_res);

    return j_res;
}

/**
 *
 * @param uuid
 * @return
 */
struct ast_json* get_dl_master_info(const char* uuid)
{
    char* sql;
    struct ast_json* j_res;
    db_res_t* db_res;

    if(uuid == NULL) {
        ast_log(LOG_WARNING, "Invalid input paramters.\n");
        return NULL;
    }

    ast_asprintf(&sql, "select * from dial_list_ma where uuid = \"%s\";", uuid);

    db_res = db_query(sql);
    ast_free(sql);
    if(db_res == NULL) {
        ast_log(LOG_ERROR, "Could not get dial_list_ma info. uuid[%s]\n", uuid);
        return NULL;
    }

    j_res = db_get_record(db_res);
    db_free(db_res);

    return j_res;
}

/**
 * Update campaign status info.
 * @param uuid
 * @param status
 * @return
 */
static int update_campaign_info_status(const char* uuid, const char* status)
{
    char* sql;
    int ret;

    if((uuid == NULL) || (status == NULL)) {
        ast_log(LOG_WARNING, "Invalid input parameters.\n");
        return false;
    }

    ast_asprintf(&sql, "update campaign set status = \"%s\" where uuid = \"%s\";", status, uuid );
    ret = db_exec(sql);
    ast_free(sql);
    if(ret == false) {
        ast_log(LOG_ERROR, "Could not update campaign status info. uuid[%s], status[%s]\n", uuid, status);
        return false;
    }

    return true;
}

/**
 *
 * @param j_camp
 * @param j_plan
 */
static void dial_desktop(const struct ast_json* j_camp, const struct ast_json* j_plan, const struct ast_json* j_dlma)
{
    return;
}

/**
 *
 * @param j_camp
 * @param j_plan
 */
static void dial_power(const struct ast_json* j_camp, const struct ast_json* j_plan, const struct ast_json* j_dlma)
{
    return;
}

/**
 *  Make a call by predictive algorithms.
 *  Currently, just consider ready agent only.
 * @param j_camp    campaign info
 * @param j_plan    plan info
 * @param j_dlma    dial list master info
 */
static void dial_predictive(struct ast_json* j_camp, struct ast_json* j_plan, struct ast_json* j_dlma)
{
    int ret;
    struct ast_json* j_dl_list;
    struct ast_json* j_dial;
    struct ast_json* j_dialing;
    struct ast_json* j_dl_update;
    char    try_cnt[128];   // string buffer for "trycnt_1"...
    char* tmp;

    // get dl_list info to dial.
    j_dl_list = get_dl_available_predictive(j_dlma, j_plan);
//    j_dl_list = get_dl_dial_available(j_dlma, "predictive");
    if(j_dl_list == NULL)
    {
        // No available list
        return;
    }

    // check available outgoing call.
    ret = check_dial_avaiable(j_camp, j_plan, j_dlma);
    if(ret == false)
    {
        // No available outgoing call.
        ast_json_unref(j_dl_list);
        return;
    }

    j_dialing = create_dialing_info(j_camp, j_plan, j_dlma, j_dl_list);
    ast_json_unref(j_dl_list);
    if(j_dialing == NULL)
    {
        ast_log(LOG_DEBUG, "Could not create dialing info.");
        return;
    }

    // create dial
    j_dial = create_dial_info(j_dialing);
    if(j_dial == NULL)
    {
        ast_log(LOG_ERROR, "Could not create dial info.");
        ast_json_unref(j_dialing);

        return;
    }
    ast_log(LOG_NOTICE, "Originating. camp_uuid[%s], camp_name[%s], channel[%s], chan_unique_id[%s], timeout[%s]",
            ast_json_string_get(ast_json_object_get(j_camp, "uuid")),
            ast_json_string_get(ast_json_object_get(j_camp, "name")),
            ast_json_string_get(ast_json_object_get(j_dial, "Channel")),
            ast_json_string_get(ast_json_object_get(j_dial, "ChannelId")),
            ast_json_string_get(ast_json_object_get(j_dial, "Timeout"))
            );

    // dial to customer
    ret = cmd_originate(j_dial);
    ast_json_unref(j_dial);
    if(ret == false)
    {
        ast_log(LOG_ERROR, "Could not originate.");
        ast_json_unref(j_dialing);

        return;
    }

    // set utc timestamp
    tmp = get_utc_timestamp();
    ast_json_object_set(j_dialing, "tm_dial", ast_json_string_create(tmp));
    ast_free(tmp);


    // create update dl_list
    sprintf(try_cnt, "trycnt_%d", (int)ast_json_integer_get(ast_json_object_get(j_dialing, "dial_index")));
    j_dl_update = ast_json_pack("{s:s, s:i, s:s, s:s, s:s, s:s}",
            "status",                   "dialing",
            try_cnt,                    ast_json_integer_get(ast_json_object_get(j_dialing, "dial_trycnt")) + 1,
            "dialing_camp_uuid",        ast_json_string_get(ast_json_object_get(j_dialing, "camp_uuid")),
            "dialing_chan_unique_id",   ast_json_string_get(ast_json_object_get(j_dialing, "chan_unique_id")),
            "tm_last_dial",             ast_json_string_get(ast_json_object_get(j_dialing, "tm_dial")),
            "uuid",                     ast_json_string_get(ast_json_object_get(j_dialing, "dl_uuid"))
            );
    if(j_dl_update == NULL)
    {
        ast_log(LOG_ERROR, "Could not create dl update info json.");
        ast_json_unref(j_dialing);
        return;
    }

    // update dl_list
//    ret = update_dl_list(ast_json_string_get(ast_json_object_get(j_dlma, "dl_table")), j_dl_update);
//    ast_json_unref(j_dl_update);
//    if(ret == false)
//    {
//        ast_json_unref(j_dialing);
//        ast_log(LOG_ERROR, "Could not update dial list info.");
//        return;
//    }
//
//    // insert dialing
//    ast_log(LOG_NOTICE, "Insert new dialing. chan_unique_id[%s]", ast_json_string_get(ast_json_object_get(j_dialing, "chan_unique_id")));
//    ret = memdb_insert("dialing", j_dialing);
//    ast_json_unref(j_dialing);
//    if(ret == false)
//    {
//        ast_log(LOG_ERROR, "Could not insert dialing info.");
//        return;
//    }

    return;
}

/**
 *
 * @param j_camp
 * @param j_plan
 */
static void dial_robo(const struct ast_json* j_camp, const struct ast_json* j_plan, const struct ast_json* j_dlma)
{
    return;
}

/**
 *  Redirect call to other dialplan.
 * @param j_camp    campaign info
 * @param j_plan    plan info
 * @param j_dlma    dial list master info
 */
static void dial_redirect(const struct ast_json* j_camp, const struct ast_json* j_plan, const struct ast_json* j_dlma)
{
    return;
}

/**
 * Get dl_list from database.
 * @param j_dlma
 * @param j_plan
 * @return
 */
static struct ast_json* get_dl_available_predictive(const struct ast_json* j_dlma, const struct ast_json* j_plan)
{
    char* sql;
    db_res_t* db_res;
    struct ast_json* j_res;

    ast_asprintf(&sql, "select "
            "*, "
            "trycnt_1 + trycnt_2 + trycnt_3 + trycnt_4 + trycnt_5 + trycnt_6 + trycnt_7 + trycnt_8 as trycnt, "
            "case when number_1 is null then 0 when trycnt_1 < %d then 1 else 0 end as num_1, "
            "case when number_2 is null then 0 when trycnt_2 < %d then 1 else 0 end as num_2, "
            "case when number_3 is null then 0 when trycnt_3 < %d then 1 else 0 end as num_3, "
            "case when number_4 is null then 0 when trycnt_4 < %d then 1 else 0 end as num_4, "
            "case when number_5 is null then 0 when trycnt_5 < %d then 1 else 0 end as num_5, "
            "case when number_6 is null then 0 when trycnt_6 < %d then 1 else 0 end as num_6, "
            "case when number_7 is null then 0 when trycnt_7 < %d then 1 else 0 end as num_7, "
            "case when number_8 is null then 0 when trycnt_8 < %d then 1 else 0 end as num_8 "
            "from %s "
            "having "
            "status = \"idle\" "
            "and num_1 + num_2 + num_3 + num_4 + num_5 + num_6 + num_7 + num_8 > 0 "
            "order by trycnt asc "
            "limit 1"
            ";",
            (int)ast_json_integer_get(ast_json_object_get(j_plan, "max_retry_cnt_1")),
            (int)ast_json_integer_get(ast_json_object_get(j_plan, "max_retry_cnt_2")),
            (int)ast_json_integer_get(ast_json_object_get(j_plan, "max_retry_cnt_3")),
            (int)ast_json_integer_get(ast_json_object_get(j_plan, "max_retry_cnt_4")),
            (int)ast_json_integer_get(ast_json_object_get(j_plan, "max_retry_cnt_5")),
            (int)ast_json_integer_get(ast_json_object_get(j_plan, "max_retry_cnt_6")),
            (int)ast_json_integer_get(ast_json_object_get(j_plan, "max_retry_cnt_7")),
            (int)ast_json_integer_get(ast_json_object_get(j_plan, "max_retry_cnt_8")),
            ast_json_string_get(ast_json_object_get(j_dlma, "dl_table"))
            );

    db_res = db_query(sql);
    ast_free(sql);
    if(db_res == NULL) {
        ast_log(LOG_ERROR, "Could not get dial list info.");
        return NULL;
    }

    j_res = db_get_record(db_res);
    db_free(db_res);
    if(j_res == NULL) {
        return NULL;
    }

    return j_res;
}

/**
 * Return dialing availability.
 * If can dialing returns true, if not returns false.
 * @param j_camp
 * @param j_plan
 * @return
 */
static int check_dial_avaiable(const struct ast_json* j_camp, const struct ast_json* j_plan, const struct ast_json* j_dlma)
{
    char* sql;
    db_res_t* db_res;
    struct ast_json* j_tmp;
    int cnt_agent;
    int cnt_dialing;

    // get count of currently available agents.
    ast_asprintf(&sql, "select count(*) from agent where status = \"%s\" and id = (select agent_uuid from agent_group where group_uuid = \"%s\");",
            "ready",
            ast_json_string_get(ast_json_object_get(j_camp, "agent_group"))
            );

    db_res = db_query(sql);
    ast_free(sql);
    if(db_res == NULL) {
        ast_log(LOG_ERROR, "Could not get available agent count.\n");
        return false;
    }

    j_tmp = db_get_record(db_res);
    db_free(db_res);
    if(j_tmp == NULL) {
        // shouldn't be reach to here.
        ast_log(LOG_ERROR, "Could not get available agent count.\n");
        return false;
    }

    cnt_agent = ast_json_integer_get(ast_json_object_get(j_tmp, "count(*)"));
    ast_json_unref(j_tmp);

    // get count of currently dailings.
    ast_asprintf(&sql, "select count(*) from %s where dialing_camp_uuid = \"%s\" and status = \"%s\";",
            ast_json_string_get(ast_json_object_get(j_dlma, "dl_table")),
            ast_json_string_get(ast_json_object_get(j_camp, "uuid")),
            "dialing"
            );

    db_res = db_query(sql);
    ast_free(sql);
    if(db_res == NULL) {
        ast_log(LOG_ERROR, "Could not get dialing count.\n");
        return false;
    }

    j_tmp = db_get_record(db_res);
    if(j_tmp == NULL) {
        // shouldn't be reach to here.
        ast_log(LOG_ERROR, "Could not get dialing count.");
        return false;
    }
    db_free(db_res);

    cnt_dialing = ast_json_integer_get(ast_json_object_get(j_tmp, "count(*)"));
    ast_json_unref(j_tmp);

//    slog(LOG_DEBUG, "Check value. cnt_agent[%d], cnt_dialing[%d]", cnt_agent, cnt_dialing);
    // compare
    if(cnt_agent <= cnt_dialing) {
        return false;
    }

    return true;
}

struct ast_json* create_dialing_info(const struct ast_json* j_camp, const struct ast_json* j_plan, const struct ast_json* j_dlma, const struct ast_json* j_dl_list)
{
    return NULL;
}

struct ast_json* create_dial_info(const struct ast_json* j_camp)
{
    return NULL;
}

int cmd_originate(const struct ast_json* j_dial)
{
    return true;
}

int memdb_insert(const char* table, const struct ast_json* j_data)
{
    return true;
}

/**
 * return utc time.
 * YYYY-MM-DDTHH:mm:ssZ
 * @return
 */
char* get_utc_timestamp(void)
{
    char    timestr[128];
    char*   res;
    struct  timespec timeptr;
    time_t  tt;
    struct tm *t;

    clock_gettime(CLOCK_REALTIME, &timeptr);
    tt = (time_t)timeptr.tv_sec;
    t = gmtime(&tt);

    strftime(timestr, sizeof(timestr), "%Y-%m-%dT%H:%M:%S", t);
    ast_asprintf(&res, "%s.%ldZ", timestr, timeptr.tv_nsec);

    return res;
}

struct ast_json* cmd_queue_show(const char* name)
{
    struct ast_json* j_res;
    const char* uuid_str[AST_UUID_STR_LEN];
    char* filename;
    int fd;
    char* cmd;
    int ret;
    FILE* fp;
    char* line;
    int read;
    size_t len;

    if(name == NULL) {
        return NULL;
    }

    // open fd
    fd = open(TEMP_FILENAME, O_CREAT|O_WRONLY|O_TRUNC);

    // command
    ast_asprintf(&cmd, "queue show %s", name);
    ret = ast_cli_command(fd, cmd);
    close(fd);
    if(ret == -1) {
        return NULL;
    }

    fp = fopen(TEMP_FILENAME, "r");
    if(fp == NULL) {
        ast_log(LOG_ERROR, "Could not open file.");
        return NULL;
    }

    while ((read = getline(&line, &len, fp)) != -1) {
        ast_log(LOG_DEBUG, "Retrieved line of length %zu :\n", read);
        ast_log(LOG_DEBUG, "%s", line);
    }
    fclose(fp);

    return NULL;

}
