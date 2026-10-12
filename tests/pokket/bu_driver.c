/* Runs the cases of bu_test.c (copied unmodified from mentalfoundry/Pokket, MIT) against this core,
   one case at a time, in the order of that file's own main(). */
#define main pokket_bu_test_original_main
#include "bu_test.c"
#undef main

#include "runner.h"

#define WITH_M(fn) static void run_##fn(void *m) { fn((mock_ps1_t *)m); }
#define NO_ARG(fn) static void run_##fn(void *unused) { (void)unused; fn(); }

WITH_M(test_get_id_gives_the_values_of_a_sony_card)
WITH_M(test_a_second_command_answers)
WITH_M(test_write_sector_puts_the_data_in_flash)
WITH_M(test_read_sector_gives_the_written_data)
WITH_M(test_read_sector_dummy_bytes_are_zero)
WITH_M(test_write_sector_refuses_a_bad_checksum)
NO_ARG(test_write_sector_needs_a_small_settle_budget)
NO_ARG(test_flag_marks_a_new_card)
NO_ARG(test_a_timed_transfer_reports_the_cost_of_each_answer)
NO_ARG(test_the_time_to_ack_is_the_exact_time_of_the_answer)
NO_ARG(test_split_runs_give_the_same_machine)
NO_ARG(test_idle_skip_gives_the_same_machine)
NO_ARG(test_idle_skip_gives_the_same_boot)
NO_ARG(test_a_selection_with_no_byte_leaves_the_kernel_ready)
NO_ARG(test_the_frames_that_hold_the_unlock_addresses_take_data)
NO_ARG(test_a_write_to_frame_0_takes_the_data_and_keeps_the_serial)
NO_ARG(test_the_bios_shell_sleeps_when_undocked)
NO_ARG(test_the_kernel_does_not_sleep_while_docked)

#define RUN(fn, arg) pokket_run(#fn, run_##fn, arg)

int main(void) {
    mock_ps1_t *m;
    const char *path = bios_path();

    setvbuf(stdout, NULL, _IONBF, 0);
    m = mock_ps1_open(path, NULL);
    if (!m) {
        printf("bu_test: no usable BIOS at %s (or COM never enabled), skipping\n", path);
        return SKIP_EXIT_CODE;
    }
    printf("bu_test: BIOS %s, communication enabled\n", path);

    RUN(test_get_id_gives_the_values_of_a_sony_card, m);
    RUN(test_a_second_command_answers, m);
    RUN(test_write_sector_puts_the_data_in_flash, m);
    RUN(test_read_sector_gives_the_written_data, m);
    RUN(test_read_sector_dummy_bytes_are_zero, m);
    RUN(test_write_sector_refuses_a_bad_checksum, m);
    RUN(test_write_sector_needs_a_small_settle_budget, NULL);
    RUN(test_flag_marks_a_new_card, NULL);
    RUN(test_a_timed_transfer_reports_the_cost_of_each_answer, NULL);
    RUN(test_the_time_to_ack_is_the_exact_time_of_the_answer, NULL);
    RUN(test_split_runs_give_the_same_machine, NULL);
    RUN(test_idle_skip_gives_the_same_machine, NULL);
    RUN(test_idle_skip_gives_the_same_boot, NULL);
    RUN(test_a_selection_with_no_byte_leaves_the_kernel_ready, NULL);
    RUN(test_the_frames_that_hold_the_unlock_addresses_take_data, NULL);
    RUN(test_a_write_to_frame_0_takes_the_data_and_keeps_the_serial, NULL);
    RUN(test_the_bios_shell_sleeps_when_undocked, NULL);
    RUN(test_the_kernel_does_not_sleep_while_docked, NULL);

    mock_ps1_close(m);
    return pokket_summary("bu_test");
}
