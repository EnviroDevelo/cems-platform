/* test_main.c - runs every host test group. */
#include "tst.h"
int tst_fail, tst_pass;

void test_scan_all(void);
void test_cmdq_all(void);
void test_seqlock_all(void);
void test_monitor_all(void);
void test_diag_all(void);
void test_param_all(void);
void test_store_all(void);
void test_blob_all(void);
void test_modbus_all(void);
void test_http_all(void);

int main(void) {
    test_scan_all();
    test_cmdq_all();
    test_seqlock_all();
    test_monitor_all();
    test_diag_all();
    test_param_all();
    test_store_all();
    test_blob_all();
    test_modbus_all();
    test_http_all();
    printf("\n%d checks passed, %d failed\n", tst_pass, tst_fail);
    return tst_fail ? 1 : 0;
}
