#include "fat32fs.h"

/*
 * Module version information, required for loadable modules.
 */
char *fat32mversion = M_VERSION;

/*
 * Device flags - D_MP indicates multiprocessor safe
 */
int fat32devflag = D_MP;

int fat32fstype;

int
fat32init(struct vfssw *vswp, int fstype)
{
    cmn_err(CE_NOTE, "fat32_init: called");
    cmn_err(CE_NOTE, "fat32_init: vswp=%p", vswp);
    cmn_err(CE_NOTE, "fat32_init: vswp->vsw_name='%s'",
            vswp->vsw_name ? vswp->vsw_name : "(null)");
    cmn_err(CE_NOTE, "fat32_init: vswp->vsw_init=%p (should be %p)",
            vswp->vsw_init, fat32init);
    cmn_err(CE_NOTE, "fat32_init: vswp->vsw_vfsops=%p (should be %p)",
            vswp->vsw_vfsops, &fat32vfsops);
    cmn_err(CE_NOTE, "fat32_init: vswp->vsw_vnodeops=%p (should be %p)",
            vswp->vsw_vnodeops, &fat32vnodeops);
    cmn_err(CE_NOTE, "fat32_init: vswp->vsw_flag=0x%lx", vswp->vsw_flag);

    /* Verify pointers are correct */
    if (vswp->vsw_init != (void *)fat32init) {
        cmn_err(CE_WARN, "fat32_init: vsw_init pointer mismatch!");
    }
    if (vswp->vsw_vfsops != &fat32vfsops) {
        cmn_err(CE_WARN, "fat32_init: vsw_vfsops pointer mismatch!");
    }
    if (vswp->vsw_vnodeops != &fat32vnodeops) {
        cmn_err(CE_WARN, "fat32_init: vsw_vnodeops pointer mismatch!");
    }

    fat32fstype = fstype;
    cmn_err(CE_NOTE, "fat32_init: fstype is %d", fstype);
    cmn_err(CE_NOTE, "fat32_init: initialization successful");
    return 0;
}

int
fat32unload(void)
{
    cmn_err(CE_NOTE, "fat32_unload: called");

    return 0;
}
