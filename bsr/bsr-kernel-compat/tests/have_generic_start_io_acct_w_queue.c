#include <linux/bio.h>

/* Introduced by mainline commit 394ffa503b, available since v3.19 */

void foo(void)
{
    struct request_queue *q = NULL;

    generic_start_io_acct(q, WRITE, 0, (struct hd_struct *) NULL);
}
