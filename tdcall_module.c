#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <asm/processor.h>

#define TDCALL_TDVMCALL 0
#define VMCALL_CUSTOM_8888 0x8888

#define TDG_VP_VMCALL_SUCCESS 0x0000000000000000ULL
#define TDG_VP_VMCALL_RETRY   0x0000000000000001ULL

#define HYC3_CPUID_LEAF     0x40000100U
#define HYC3_CMD_GET_QUOTE   1
#define HYC3_CMD_SUBMIT      2
#define HYC3_CMD_WAIT_INPUT  3
#define HYC3_CMD_COMPLETE    4
#define HYC3_CMD_WAIT_RESULT 5
#define HYC3_CMD_L2_READY    6
#define HYC3_CMD_REGISTER_SHARED 7

#define HYC3_MAX_PAYLOAD       4096
#define HYC3_QUOTE_BUF_SIZE    8192
#define HYC3_SHARED_RESULT_GPA 0x00800000ULL
#define HYC3_L2_2_WORK_GPA     0x00801000ULL
#define HYC3_L2_1_INPUT_GPA    0x00802000ULL
#define HYC3_QUOTE_GPA         0x00803000ULL
#define HYC3_RETRY_DELAY_MS    10
#define HYC3_MAX_RETRIES       90000
#define HYC3_L2_1_DEFAULT_START_DELAY_MS 30000

static int is_l2_1 = -1;
static bool ready_only;
static bool autotest = true;
static unsigned int start_delay_ms;
module_param(is_l2_1, int, 0644);
MODULE_PARM_DESC(is_l2_1, "-1 auto-detect, 1 for L2-1 submitter, 0 for L2-2 worker");
module_param(ready_only, bool, 0644);
MODULE_PARM_DESC(ready_only, "Only signal L2-1 Linux/rootfs readiness; do not run shared-memory test");
module_param(autotest, bool, 0644);
MODULE_PARM_DESC(autotest, "Run the HYC3 L2-1 -> L1/SVSM -> L2-2 fixed shared-page +1 test asynchronously");
module_param(start_delay_ms, uint, 0644);
MODULE_PARM_DESC(start_delay_ms, "Delay before starting the asynchronous HYC3 shared-memory test; L2-1 defaults to 30000ms");

static u32 hyc3_get_vm_index(void)
{
    u32 eax = HYC3_CPUID_LEAF;
    u32 ebx = 0;
    u32 ecx = 0;
    u32 edx = 0;

    asm volatile("cpuid"
                 : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx)
                 :
                 : "memory");

    if (ebx == 0)
        return 0;

    return eax;
}

static u64 hyc3_tdvmcall(u64 cmd, u64 arg0, u64 arg1, u64 *out_r11, u64 *out_r12)
{
    register u64 rax asm("rax") = TDCALL_TDVMCALL;
    register u64 rcx asm("rcx") = (1ULL << 10) | (1ULL << 11) |
                                  (1ULL << 12) | (1ULL << 13) |
                                  (1ULL << 14);
    register u64 r10 asm("r10") = 0;
    register u64 r11 asm("r11") = VMCALL_CUSTOM_8888;
    register u64 r12 asm("r12") = cmd;
    register u64 r13 asm("r13") = arg0;
    register u64 r14 asm("r14") = arg1;

    asm volatile(
        ".byte 0x66, 0x0f, 0x01, 0xcc\n\t"
        : "+a"(rax), "+c"(rcx), "+r"(r10), "+r"(r11), "+r"(r12),
          "+r"(r13), "+r"(r14)
        :
        : "memory"
    );

    if (out_r11)
        *out_r11 = r11;
    if (out_r12)
        *out_r12 = r12;

    return r10;
}

static int hyc3_poll_wait_input(u64 *work_gpa, u64 *input_len)
{
    u64 ret, state = 0, len = 0;
    unsigned int i;

    for (i = 0; i < HYC3_MAX_RETRIES; i++) {
        ret = hyc3_tdvmcall(HYC3_CMD_WAIT_INPUT, 0, 0, &state, &len);
        if (ret == TDG_VP_VMCALL_SUCCESS) {
            *work_gpa = state;
            *input_len = len;
            return 0;
        }
        if (ret != TDG_VP_VMCALL_RETRY) {
            pr_err("tdcall_module: WAIT_INPUT failed ret=0x%llx state=0x%llx\n", ret, state);
            return -EIO;
        }
        if ((i % 1000) == 0)
            pr_info("tdcall_module: WAIT_INPUT retry %u state=0x%llx\n", i, state);
        msleep(HYC3_RETRY_DELAY_MS);
    }

    pr_err("tdcall_module: WAIT_INPUT timed out\n");
    return -ETIMEDOUT;
}

static int hyc3_poll_wait_result(u64 *result_len, u64 *shared_gpa)
{
    u64 ret, len = 0, shared = 0;
    unsigned int i;

    for (i = 0; i < HYC3_MAX_RETRIES; i++) {
        ret = hyc3_tdvmcall(HYC3_CMD_WAIT_RESULT, 0, 0, &len, &shared);
        if (ret == TDG_VP_VMCALL_SUCCESS) {
            *result_len = len;
            *shared_gpa = shared;
            return 0;
        }
        if (ret != TDG_VP_VMCALL_RETRY) {
            pr_err("tdcall_module: WAIT_RESULT failed ret=0x%llx state=0x%llx\n", ret, len);
            return -EIO;
        }
        if ((i % 1000) == 0)
            pr_info("tdcall_module: WAIT_RESULT retry %u state=0x%llx\n", i, len);
        msleep(HYC3_RETRY_DELAY_MS);
    }

    pr_err("tdcall_module: WAIT_RESULT timed out\n");
    return -ETIMEDOUT;
}


static int hyc3_register_shared_page(void)
{
    u64 ret, registered_gpa = 0;

    ret = hyc3_tdvmcall(HYC3_CMD_REGISTER_SHARED, HYC3_SHARED_RESULT_GPA,
                        HYC3_MAX_PAYLOAD, &registered_gpa, NULL);
    if (ret != TDG_VP_VMCALL_SUCCESS) {
        pr_err("tdcall_module: REGISTER_SHARED failed ret=0x%llx gpa=0x%llx\n",
               ret, HYC3_SHARED_RESULT_GPA);
        return -EIO;
    }

    pr_info("tdcall_module: fixed shared GPA registered 0x%llx\n", registered_gpa);
    return 0;
}

static void hyc3_signal_l2_1_ready(void)
{
    u64 ret;

    ret = hyc3_tdvmcall(HYC3_CMD_L2_READY, 0, 0, NULL, NULL);
    if (ret == TDG_VP_VMCALL_SUCCESS)
        pr_info("tdcall_module: L2-1 signaled Linux/initramfs readiness to SVSM\n");
    else
        pr_warn("tdcall_module: L2-1 ready signal ret=0x%llx\n", ret);
}

static int hyc3_l2_1_run(void)
{
    void *result_map = NULL;
    u64 ret, result_len = 0, shared_gpa = 0;
    u64 input = 41;
    u64 result = 0;
    int rc = 0;

    hyc3_signal_l2_1_ready();

    rc = hyc3_register_shared_page();
    if (rc)
        goto out;

    pr_info("tdcall_module: L2-1 mapping registered shared GPA 0x%llx\n", HYC3_SHARED_RESULT_GPA);
    result_map = memremap(HYC3_SHARED_RESULT_GPA, HYC3_MAX_PAYLOAD, MEMREMAP_WB);
    if (!result_map) {
        pr_err("tdcall_module: L2-1 memremap failed shared=%p\n", result_map);
        rc = -ENOMEM;
        goto out;
    }

    memset(result_map, 0, HYC3_MAX_PAYLOAD);
    memcpy(result_map, &input, sizeof(input));
    wmb();

    pr_info("tdcall_module: quote path skipped during rootfs bring-up\n");
    ret = hyc3_tdvmcall(HYC3_CMD_SUBMIT, HYC3_SHARED_RESULT_GPA, sizeof(input), NULL, NULL);
    if (ret != TDG_VP_VMCALL_SUCCESS) {
        pr_err("tdcall_module: SUBMIT failed ret=0x%llx\n", ret);
        rc = -EIO;
        goto out;
    }
    pr_info("tdcall_module: L2-1 submitted input=%llu shared_gpa=0x%llx\n",
            input, HYC3_SHARED_RESULT_GPA);

    rc = hyc3_poll_wait_result(&result_len, &shared_gpa);
    if (rc)
        goto out;

    rmb();
    memcpy(&result, result_map, sizeof(result));
    pr_info("tdcall_module: L2-1 got result=%llu len=%llu shared_gpa=0x%llx\n",
            result, result_len, shared_gpa);

    if (shared_gpa != HYC3_SHARED_RESULT_GPA || result != input + 1) {
        pr_err("tdcall_module: unexpected result=%llu expected=%llu shared=0x%llx\n",
               result, input + 1, shared_gpa);
        rc = -EIO;
    } else {
        pr_info("tdcall_module: HYC3 shared-memory +1 test PASSED\n");
    }

out:
    /* HYC3: keep fixed-GPA MEMREMAP mappings alive during bring-up.
     * memunmap() can block in the current TDP alias path after the
     * shared-page test has already completed.
     */
    pr_info("tdcall_module: L2-1 init returning rc=%d\n", rc);
    return rc;
}

static int hyc3_l2_2_run(void)
{
    void *work_map = NULL;
    u64 ret, work_gpa = 0, input_len = 0;
    u64 input = 0;
    u64 result = 0;
    int rc = 0;

    pr_info("tdcall_module: L2-2 waiting for L2-1 input...\n");
    rc = hyc3_poll_wait_input(&work_gpa, &input_len);
    if (rc)
        goto out;

    if ((work_gpa != HYC3_SHARED_RESULT_GPA) || (input_len < sizeof(input)) ||
        (input_len > HYC3_MAX_PAYLOAD)) {
        pr_err("tdcall_module: invalid work buffer gpa=0x%llx len=%llu\n", work_gpa, input_len);
        rc = -EINVAL;
        goto out;
    }

    work_map = memremap(work_gpa, HYC3_MAX_PAYLOAD, MEMREMAP_WB);
    if (!work_map) {
        pr_err("tdcall_module: L2-2 memremap failed shared=%p\n", work_map);
        rc = -ENOMEM;
        goto out;
    }

    rmb();
    memcpy(&input, work_map, sizeof(input));
    result = input + 1;
    memset(work_map, 0, HYC3_MAX_PAYLOAD);
    memcpy(work_map, &result, sizeof(result));
    wmb();

    pr_info("tdcall_module: L2-2 computed %llu + 1 = %llu\n", input, result);
    ret = hyc3_tdvmcall(HYC3_CMD_COMPLETE, HYC3_SHARED_RESULT_GPA, sizeof(result), NULL, NULL);
    if (ret != TDG_VP_VMCALL_SUCCESS) {
        pr_err("tdcall_module: COMPLETE failed ret=0x%llx\n", ret);
        rc = -EIO;
    } else {
        pr_info("tdcall_module: L2-2 published result to shared GPA 0x%llx\n", HYC3_SHARED_RESULT_GPA);
    }

out:
    if (work_map)
        memunmap(work_map);

    return rc;
}


static struct workqueue_struct *hyc3_wq;
static bool hyc3_async_run_l2_1;

static void hyc3_async_workfn(struct work_struct *work)
{
    int rc;

    (void)work;
    rc = hyc3_async_run_l2_1 ? hyc3_l2_1_run() : hyc3_l2_2_run();
    pr_info("tdcall_module: async shared-memory test role=%s finished rc=%d\n",
            hyc3_async_run_l2_1 ? "L2-1" : "L2-2", rc);
}

static DECLARE_DELAYED_WORK(hyc3_async_work, hyc3_async_workfn);

static int __init tdcall_test_init(void)
{
    u32 vm_index = hyc3_get_vm_index();
    bool run_l2_1 = is_l2_1 >= 0 ? !!is_l2_1 : vm_index == 0;
    unsigned int delay = start_delay_ms;

    pr_info("tdcall_module: loaded vm_index=%u role=%s override=%d ready_only=%d autotest=%d start_delay_ms=%u\n",
            vm_index, run_l2_1 ? "L2-1" : "L2-2", is_l2_1, ready_only, autotest,
            start_delay_ms);

    if (ready_only || !autotest) {
        if (run_l2_1)
            hyc3_signal_l2_1_ready();
        pr_info("tdcall_module: ready_only/autotest-off complete vm_index=%u role=%s ready_only=%d autotest=%d\n",
                vm_index, run_l2_1 ? "L2-1" : "L2-2", ready_only, autotest);
        return 0;
    }

    if (run_l2_1) {
        hyc3_signal_l2_1_ready();
        if (delay == 0)
            delay = HYC3_L2_1_DEFAULT_START_DELAY_MS;
    }

    hyc3_async_run_l2_1 = run_l2_1;
    hyc3_wq = alloc_workqueue("hyc3_tdcall_wq", WQ_UNBOUND, 1);
    if (!hyc3_wq)
        return -ENOMEM;

    queue_delayed_work(hyc3_wq, &hyc3_async_work, msecs_to_jiffies(delay));
    pr_info("tdcall_module: queued async shared-memory test role=%s delay_ms=%u\n",
            run_l2_1 ? "L2-1" : "L2-2", delay);
    return 0;
}

static void __exit tdcall_test_exit(void)
{
    if (hyc3_wq) {
        cancel_delayed_work_sync(&hyc3_async_work);
        destroy_workqueue(hyc3_wq);
    }
    pr_info("tdcall_module: unloaded\n");
}
module_init(tdcall_test_init);
module_exit(tdcall_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HYC3 TDX/SVSM fixed-control-GPA shared-memory bring-up test");
