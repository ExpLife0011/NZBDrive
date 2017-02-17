/*
    Copyright (c) 2017, Ole Stauning
    All rights reserved.

    Use of this source code is governed by a GPLv3-style license that can be found
    in the LICENSE file.
*/

#include "NZBFuseDrive.hpp"
#include "Logger.hpp"
#include "IDirectory.hpp"

#define FUSE_USE_VERSION 26
#include <fuse.h>


namespace
{

using namespace ByteFountain;


int nzb_getattr(const char *path, struct stat *stbuf)
{
//	std::cout<<"NZB_GETATTR (START): "<<path<<std::endl;
	fuse_context* pcontext=fuse_get_context();
	IDirectory& nzbdir(*static_cast<IDirectory*>(pcontext->private_data));

	memset(stbuf, 0, sizeof(struct stat));
	
	if (strcmp(path, "/") == 0)
	{
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
//		std::cout<<"NZB_GETATTR (STOP)"<<std::endl;
		return 0;
	} 
	
	std::shared_ptr<IFile> file=nzbdir.GetFile(path);
	
	if (file)
	{
		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_nlink = 1;
		stbuf->st_size = file->GetFileSize();
		return 0;
	}
	else
	{
		std::shared_ptr<IDirectory> dir=nzbdir.GetDirectory(path);
		if (!dir) return -ENOENT;
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	}
	
//	std::cout<<"NZB_GETATTR (STOP)"<<std::endl;
	return 0;
}

int nzb_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
//	std::cout<<"NZB_READDIR (START): "<<path<<std::endl;
	fuse_context* pcontext=fuse_get_context();
	IDirectory& nzbdir(*static_cast<IDirectory*>(pcontext->private_data));

	(void) offset;
	(void) fi;

	std::shared_ptr<IDirectory> dir=nzbdir.GetDirectory(path);
	
	if (dir.get()!=&nzbdir)
	{
		filler(buf, ".", NULL, 0);
		filler(buf, "..", NULL, 0);
	}
	
	std::vector< std::pair<boost::filesystem::path,ContentType> > content=dir->GetContent();
	
	for(auto element : content)
	{
//		std::cout<<element.first.string()<<std::endl;
		if (filler(buf, element.first.string().c_str(), NULL, 0)) break;
	}

//	std::cout<<"NZB_READDIR (STOP)"<<std::endl;
	return 0;
}

int nzb_open(const char *path, struct fuse_file_info *fi)
{
	fuse_context* pcontext=fuse_get_context();
	IDirectory& nzbdir(*static_cast<IDirectory*>(pcontext->private_data));

	std::shared_ptr<IFile> file=nzbdir.GetFile(path);
	
	if (!file) return -ENOENT;
	
	if ((fi->flags & 3) != O_RDONLY)
		return -EACCES;

	return 0;

}

//char dummy[5*1024*1024];

int nzb_read(const char *path, char *buf, size_t size, off_t offset,
		      struct fuse_file_info *fi)
{
	fuse_context* pcontext=fuse_get_context();
	IDirectory& nzbdir(*static_cast<IDirectory*>(pcontext->private_data));
	
	size_t readsize;
	
	std::shared_ptr<IFile> file=nzbdir.GetFile(path);
	
	if (!file) return -errno;
	
	size_t ulreadsize;
	bool err=file->GetFileData(buf,offset,size,ulreadsize);
	if (err) return -errno;
	readsize=ulreadsize;
	
	// CACHE-AHEAD REENGINEERING:
//	file->AsyncGetFileData([](const size_t readsize){},dummy,offset+size,5*1024*1024);
	
	return readsize;
}

void* nzb_init(struct fuse_conn_info *conn)
{
	fuse_context* pcontext=fuse_get_context();
	IDirectory* nzbdir(static_cast<IDirectory*>(pcontext->private_data));
	return nzbdir;
}

void nzb_destroy(void* p)
{
}
	
	
struct nzb_fuse_operations: fuse_operations 
{
	nzb_fuse_operations ()
	{
		getattr=nzb_getattr;
		readdir=nzb_readdir;
		open=nzb_open;
		read=nzb_read;
		init=nzb_init;
		destroy=nzb_destroy;
	}
}; //class

nzb_fuse_operations nzb_oper;//object

}

namespace ByteFountain
{
	NZBFuseDrive::NZBFuseDrive(const boost::filesystem::path& cache_path, Logger& log): 
		NZBDrive(log),m_fuse(0),m_ch(0),m_cache_path(cache_path),m_drivepath(),m_fuseThread(),m_log(log)
	{}

	void NZBFuseDrive::Start(const std::string& drivepath, const std::vector<UsenetServer>& servers)
	{
		m_drivepath=drivepath;
	
		if (!boost::filesystem::exists("/dev/fuse"))
		{
			m_log<<Logger::Error<<"Could not find /dev/fuse. Make sure the fuse module is loaded"<<Logger::End;
			throw std::runtime_error("Could not find /dev/fuse. Make sure the fuse module is loaded");
		}
		
		int res;

		int argc=3;
		
		char arg1[]="nzbmounter";
		char arg2[]="-o";
		char arg3[]="allow_other";
		char *argv[]={arg1,arg2,arg3};
		
		struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
		
		m_ch = fuse_mount(drivepath.c_str(), &args);
		if (!m_ch) 
		{
			fuse_opt_free_args(&args);
			goto err_unmount;
		}

		m_fuse = fuse_new(m_ch, &args, &nzb_oper, sizeof(nzb_fuse_operations), static_cast<void*>(GetRootDir().get()));
		
		fuse_opt_free_args(&args);
		
		if (m_fuse == NULL)
			goto err_unmount;

		res = fuse_daemonize(true);
		if (res == -1)
			goto err_unmount;

//		res = fuse_set_signal_handlers(fuse_get_session(m_fuse));
//		if (res == -1)
//			goto err_unmount;
		
		m_fuseThread=std::thread( 
			[this]()
			{ 
				try
				{
					m_log<<Logger::Debug<<"Entering Fuse loop"<<Logger::End;
					fuse_loop(m_fuse);
					m_log<<Logger::Debug<<"Exiting Fuse loop"<<Logger::End;
				}
				catch(...)
				{
					m_log<<Logger::Fatal<<"Fuse loop threw an exception"<<Logger::End;
					throw;
				}
			} 
		);
		
		NZBDrive::SetCachePath(m_cache_path);
		
		NZBDrive::Start();
		
		for(const auto& server : servers) NZBDrive::AddServer(server);
		
		return;

err_unmount:	{
			if (m_ch) fuse_unmount(m_drivepath.c_str(), m_ch);
			if (m_fuse) fuse_destroy(m_fuse);
			std::ostringstream oss;
			oss<<"Could not mount directory: \""<<drivepath<<"\"";
			throw std::runtime_error(oss.str());
		}

		return;
	}
	void NZBFuseDrive::FusermountCmd()
	{
		std::string cmd("fusermount -u -z "+m_drivepath);
		if (::system(cmd.c_str())==-1)
		{
			m_log<<Logger::Error<<"Command \"fusermount\" failed"<<Logger::End;			
		}
	}
	void NZBFuseDrive::Stop()
	{
		m_log<<Logger::Debug<<"Stopping NZBFuseDrive"<<Logger::End;
		FusermountCmd();
		m_fuseThread.join();
		fuse_unmount(m_drivepath.c_str(), m_ch);
		if (m_fuse) fuse_destroy(m_fuse);
		
		NZBDrive::Stop();
	}
}




