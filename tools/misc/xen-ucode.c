#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <xenctrl.h>

#include <xen-tools/libs.h>

static xc_interface *xch;

static const char intel_id[] = "GenuineIntel";
static const char   amd_id[] = "AuthenticAMD";

struct xenpf_old_pcpu_version {
    /* IN */
    uint32_t xen_cpuid;
    /* OUT */
    /* The maxium cpu_id that is present */
    uint32_t max_present;
    char vendor_id[12];
    uint32_t family;
    uint32_t model;
    uint32_t stepping;
    uint32_t cpu_signature;    /* ABI delta */
    uint32_t pf;               /* |         */
    uint32_t ucode_revision;   /* |         */
};

/*
 * The XenServer patchqueue, prior to the patch which introduced this change,
 * extended 'struct xenpf_pcpu_version' with cpu_signature, pf and
 * ucode_revision.  Upstream, this information is provided by
 * XENPF_get_ucode_revision instead.
 *
 * Provide a local xc_get_cpu_version() which copies more data out of the
 * xen_platform_op union, so the data can be examined if
 * xc_get_ucode_revision() doesn't appear to exist.
 */
static int xc_get_old_cpu_version(xc_interface *xch, struct xenpf_old_pcpu_version *cpu_ver)
{
    int ret;
    struct xen_platform_op op = {
        .cmd = XENPF_get_cpu_version,
        .u.pcpu_version.xen_cpuid = cpu_ver->xen_cpuid,
    };

    ret = xc_platform_op(xch, &op);
    if ( ret != 0 )
        return ret;

    BUILD_BUG_ON(sizeof(struct xenpf_pcpu_version) != 32);
    BUILD_BUG_ON(sizeof(struct xenpf_old_pcpu_version) != 44);
    BUILD_BUG_ON(sizeof(struct xenpf_old_pcpu_version) > sizeof(op.u));

    memcpy(cpu_ver, &op.u, sizeof(*cpu_ver));

    return 0;
}

static void show_curr_cpu(FILE *f)
{
    int ret;
    struct xenpf_old_pcpu_version cpu_ver = { .xen_cpuid = 0 };
    struct xenpf_ucode_revision ucode_rev = { .cpu = 0 };
    /* Always exit with 2 when called during usage-info */
    int exit_code = (f == stderr) ? 2 : 1;

    ret = xc_get_old_cpu_version(xch, &cpu_ver);
    if ( ret )
    {
        fprintf(stderr, "Failed to get CPU information. (err: %s)\n",
                strerror(errno));
        exit(exit_code);
    }

    ret = xc_get_ucode_revision(xch, &ucode_rev);
    if ( ret )
    {
        if ( errno == ENOSYS &&
             cpu_ver.cpu_signature && cpu_ver.ucode_revision )
        {
            ucode_rev.signature = cpu_ver.cpu_signature;
            ucode_rev.pf = cpu_ver.pf;
            ucode_rev.revision = cpu_ver.ucode_revision;
            goto got_details;
        }

        fprintf(stderr, "Failed to get microcode information. (err: %s)\n",
                strerror(errno));
        exit(exit_code);
    }

 got_details:
    /*
     * Print signature in a form that allows to quickly identify which ucode
     * blob to load, e.g.:
     *
     *      Intel:   /lib/firmware/intel-ucode/06-55-04
     *      AMD:     /lib/firmware/amd-ucode/microcode_amd_fam19h.bin
     */
    if ( memcmp(cpu_ver.vendor_id, intel_id,
                sizeof(cpu_ver.vendor_id)) == 0 )
    {
        fprintf(f,
                "CPU signature %02x-%02x-%02x (raw 0x%08x) pf %#x revision 0x%08x\n",
                cpu_ver.family, cpu_ver.model, cpu_ver.stepping,
                ucode_rev.signature, ucode_rev.pf, ucode_rev.revision);
    }
    else if ( memcmp(cpu_ver.vendor_id, amd_id,
                     sizeof(cpu_ver.vendor_id)) == 0 )
    {
        fprintf(f,
                "CPU signature %02x-%02x-%02x (raw 0x%08x) revision 0x%08x\n",
                cpu_ver.family, cpu_ver.model, cpu_ver.stepping,
                ucode_rev.signature, ucode_rev.revision);
    }
    else
    {
        fprintf(f, "Unsupported CPU vendor: %s\n", cpu_ver.vendor_id);
        exit(exit_code);
    }
}

int main(int argc, char *argv[])
{
    int fd, ret;
    char *filename, *buf;
    size_t len;
    struct stat st;

    xch = xc_interface_open(NULL, NULL, 0);
    if ( xch == NULL )
    {
        fprintf(stderr, "Error opening xc interface. (err: %s)\n",
                strerror(errno));
        exit(1);
    }

    if ( argc < 2 )
    {
        fprintf(stderr,
                "xen-ucode: Xen microcode updating tool\n"
                "Usage: %s [<microcode file> | show-cpu-info]\n", argv[0]);
        show_curr_cpu(stderr);
        exit(2);
    }

    if ( !strcmp(argv[1], "show-cpu-info") )
    {
        show_curr_cpu(stdout);
        return 0;
    }

    filename = argv[1];
    fd = open(filename, O_RDONLY);
    if ( fd < 0 )
    {
        fprintf(stderr, "Could not open %s. (err: %s)\n",
                filename, strerror(errno));
        exit(1);
    }

    if ( fstat(fd, &st) != 0 )
    {
        fprintf(stderr, "Could not get the size of %s. (err: %s)\n",
                filename, strerror(errno));
        exit(1);
    }

    len = st.st_size;
    buf = mmap(0, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if ( buf == MAP_FAILED )
    {
        fprintf(stderr, "mmap failed. (error: %s)\n", strerror(errno));
        exit(1);
    }

    ret = xc_microcode_update(xch, buf, len);
    if ( ret )
    {
        fprintf(stderr, "Failed to update microcode. (err: %s)\n",
                strerror(errno));
        exit(1);
    }

    xc_interface_close(xch);

    if ( munmap(buf, len) )
    {
        printf("Could not unmap: %d(%s)\n", errno, strerror(errno));
        exit(1);
    }
    close(fd);

    return 0;
}
