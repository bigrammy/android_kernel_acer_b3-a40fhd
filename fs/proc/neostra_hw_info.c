/*author: huangjianlong 20170320*/

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>

static DEFINE_SPINLOCK(hw_info_access);

static char hw_info[1024]="hw_info:";


void neostra_add_hw_info(char *item, char *str, unsigned int val)
{
	char temp[10];
	
	if(NULL == str){		
		sprintf(temp, "0x%x", val);
	}
	
	spin_lock(&hw_info_access);
	
	strcat(hw_info, " ");
	strcat(hw_info, item);
	strcat(hw_info, "=");
	
	if(NULL == str){
		strcat(hw_info, temp);
	}else{
		strcat(hw_info, str);
	}
	spin_unlock(&hw_info_access);
}
EXPORT_SYMBOL(neostra_add_hw_info);


static int hw_info_proc_show(struct seq_file *m, void *v)
{
	//neostra_add_hw_info("lcd-id", "0x1234");//for test
	
	spin_lock(&hw_info_access);
	seq_printf(m, "%s\n", hw_info);
	spin_unlock(&hw_info_access);
	return 0;
}

static int hw_info_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, hw_info_proc_show, NULL);
}

static const struct file_operations hw_info_proc_fops = {
	.open		= hw_info_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init proc_hw_info_init(void)
{
	proc_create("neostra_hw_info", 0, NULL, &hw_info_proc_fops);
	return 0;
}

fs_initcall(proc_hw_info_init);
