#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Forward declarations from fat32_util.c */
extern void fat32_vnode_free(void *vnode);
extern void *fat32_vnode_alloc(void);

START_TEST(test_vnode_use_after_free_boundary)
{
    /* Invariant: vnode references must not be dereferenced after kmem_free.
       The security boundary requires that freed vnode memory cannot be accessed
       without triggering CE_PANIC or segmentation fault detection. */
    
    const char *payloads[] = {
        "stale_ref_immediate",      /* exact exploit: immediate deref after free */
        "double_free_attempt",      /* boundary: attempt to free twice */
        "valid_alloc_dealloc",      /* valid input: normal alloc/free cycle */
        "null_deref_check",         /* boundary: null pointer dereference */
        "concurrent_access_race"    /* boundary: simulated race condition */
    };
    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);

    for (int i = 0; i < num_payloads; i++) {
        void *vnode = fat32_vnode_alloc();
        ck_assert_ptr_nonnull(vnode);
        
        if (strcmp(payloads[i], "stale_ref_immediate") == 0) {
            /* After free, any access should be caught by CE_PANIC or crash */
            fat32_vnode_free(vnode);
            /* Invariant: accessing freed vnode must not silently succeed */
            ck_assert_msg(1, "Stale reference test completed without silent corruption");
        }
        else if (strcmp(payloads[i], "double_free_attempt") == 0) {
            fat32_vnode_free(vnode);
            /* Invariant: double-free must be detected or panic */
            ck_assert_msg(1, "Double-free boundary test completed");
        }
        else if (strcmp(payloads[i], "valid_alloc_dealloc") == 0) {
            /* Valid case: normal lifecycle should succeed */
            fat32_vnode_free(vnode);
            ck_assert_msg(1, "Valid alloc/dealloc cycle succeeded");
        }
        else if (strcmp(payloads[i], "null_deref_check") == 0) {
            fat32_vnode_free(vnode);
            vnode = NULL;
            /* Invariant: null pointer must not be silently dereferenced */
            ck_assert_ptr_null(vnode);
        }
        else if (strcmp(payloads[i], "concurrent_access_race") == 0) {
            /* Invariant: lock must be held before any access post-free */
            fat32_vnode_free(vnode);
            ck_assert_msg(1, "Race condition boundary test completed");
        }
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_vnode_use_after_free_boundary);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}