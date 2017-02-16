/*
    Copyright (c) 2017, Ole Stauning
    All rights reserved.

    Use of this source code is governed by a GPLv3-style license that can be found
    in the LICENSE file.
*/

#define _CRT_SECURE_NO_WARNINGS

#include "NZBDrive.hpp"
#include "NZBFile.hpp"
#include "NZBDirectory.hpp"
#include "Logger.hpp"
#include "NZBDriveMounter.hpp"
#include "nzb_fileloader.hpp"
#include "Logger.hpp"
#include "ReadAheadFile.hpp"
#include "NZBDriveIMPL.hpp"
#include <thread>

namespace ByteFountain
{

	namespace /*anonymous*/
	{

		template <typename... TArgs>
		std::function<void(TArgs...)> RelayEvent(boost::asio::io_service::strand& strnd, std::function<void(TArgs...)>& handler)
		{
			return [handler, &strnd](TArgs... args)
			{
				strnd.post(
					std::bind(
					[handler](TArgs... args)
				{
					if (handler) handler(args...);
				}, args...)
					);
			};
		}

		template <typename T, typename... TArgs>
		static void SendEvent(T& handler, TArgs&&... args)
		{
			auto h = handler;
			if (h) h(std::forward<TArgs>(args)...);
		}

		bool expand_user(boost::filesystem::path& bpath)
		{
			std::string path = bpath.string();
			if (!path.empty() && path[0] == '~')
			{
				if (std::getenv("HOME"))
				{
					const char* hdrive = std::getenv("HOMEDRIVE");
					if (!hdrive) return false;
					const char* hpath = std::getenv("HOMEPATH");
					if (!hpath) return false;
					path.replace(0, 1, std::string(hdrive) + hpath);
				}
				else
				{
					const char* home = std::getenv("HOMEPATH");
					if (!home) return false;
					path.replace(0, 1, home);
				}
			}
			bpath = path;
			return true;
		}

	}


	NZBDriveIMPL::NZBDriveIMPL() : NZBDriveIMPL(nullptr) {}
	NZBDriveIMPL::NZBDriveIMPL(Logger& log) : NZBDriveIMPL(&log) {}



	NZBDriveIMPL::NZBDriveIMPL(Logger* logger) :
		m_defaultLogger(std::cerr),
		m_logger(logger ? *logger : m_defaultLogger),
		m_state(Stopped),
		m_mutex(),
		m_mountStates(),
		m_registered(false),
		m_io_service(),
		m_limiter(m_io_service, m_logger, [this](){ return GetRateLimiterConfiguration(); }),
		m_LastDriveRXBytes(0),
		m_clients(m_io_service, m_logger, m_limiter),
		m_thread(),
		m_work(),
		m_cache_path("~/NZBDriveCache"),
		m_root_dir(new NZBDirectory()),
		m_root_rawdir(new NZBDirectory()),
		m_timer(m_io_service),
		m_duration(milliseconds(100)),
		m_log(m_logger),
		m_nzbCount(0),
		m_fileCount(0),
		m_shape(),
		m_currentThrottlingMode(m_shape.Mode),
		m_readRate(0),
		m_readAheadFile(nullptr),
		m_driveStats(),
		m_event_io_service(),
		m_event_strand(m_event_io_service),
		m_event_thread(),
		m_event_work(),
		m_nzbFileOpenFunction(),
		m_nzbFileCloseFunction(),
		m_fileAddedFunction(),
		m_fileInfoFunction(),
		m_fileSegmentStateChangedFunction(),
		m_fileRemovedFunction()
	{}

	NZBDriveIMPL::~NZBDriveIMPL()
	{
		ResetHandlers();
		m_mountStates.clear();
	}

	RateLimiter::Parameters NZBDriveIMPL::GetRateLimiterConfiguration()
	{
		std::size_t DriveRXBytes = m_driveStats.RXBytes;
		std::size_t readRate = DriveRXBytes > m_LastDriveRXBytes ? DriveRXBytes - m_LastDriveRXBytes : 0;

		m_readRate = (10 * readRate + 990 * m_readRate) / 1000;

		//		m_log << Logger::Error << DriveRXBytes << " - " << m_LastDriveRXBytes << " = " << readRate << Logger::End;

		m_LastDriveRXBytes = DriveRXBytes;

		NetworkThrottling::NetworkMode throttlingMode = m_shape.Mode;

		if (throttlingMode == NetworkThrottling::Adaptive)
		{
			const bool hasPriorityWork = m_clients.HasPriorityWork();
			const bool blockedReads = m_driveStats.BlockedReads > 0;
			const bool preCaching = m_limiter.Network.RXBytes < m_driveStats.FastReadUntil;

			if (hasPriorityWork || blockedReads || preCaching)
			{
				throttlingMode = NetworkThrottling::Constant;
			}

			if (m_currentThrottlingMode != throttlingMode)
			{
				m_currentThrottlingMode = throttlingMode;
				switch (m_currentThrottlingMode)
				{
				case NetworkThrottling::Constant:
					m_log << Logger::Info << "Throttling: Switching to Constant bitrate ( "
						<< (hasPriorityWork ? "Priority " : "")
						<< (blockedReads ? "Blocked " : "")
						<< (preCaching ? "Caching " : "")
						<< ")" << Logger::End;
					break;
				case NetworkThrottling::Adaptive:
					m_log << Logger::Info << "Throttling: Switching to Adaptive bitrate" << Logger::End;
					break;
				}
			}

		}

		m_currentThrottlingMode = throttlingMode;

		switch (m_currentThrottlingMode)
		{
		case NetworkThrottling::Constant:
			return RateLimiter::Parameters(m_shape.NetworkLimit);

		case NetworkThrottling::Adaptive:
			return RateLimiter::Parameters(
				m_shape.BackgroundNetworkRate,
				(m_shape.AdaptiveIORatioPCT * m_readRate) / 100,
				m_shape.NetworkLimit);
		}

		return RateLimiter::Parameters();
	}

	namespace
	{
		void ThrowCallBeforeStartedException()
		{
			throw std::domain_error("Method should be called before calling Start method");
		}
	}


	void NZBDriveIMPL::ResetHandlers()
	{
		m_logger.SetLoggingFunction(nullptr);
		m_clients.SetConnectionStateChangedHandler(nullptr);
		m_clients.SetConnectionInfoHandler(nullptr);
		m_nzbFileOpenFunction = nullptr;
		m_nzbFileCloseFunction = nullptr;
		m_fileAddedFunction = nullptr;
		m_fileInfoFunction = nullptr;
		m_fileSegmentStateChangedFunction = nullptr;
		m_fileRemovedFunction = nullptr;
	}
	void NZBDriveIMPL::SetLoggingHandler(LoggingFunction handler)
	{
		m_logger.SetLoggingFunction(handler);
	}
	void NZBDriveIMPL::SetConnectionStateChangedHandler(NewsClientCache::ConnectionStateChangedFunction handler)
	{
		if (m_state == Stopped)
		{
			m_clients.SetConnectionStateChangedHandler(RelayEvent(m_event_strand, handler));
		}
		else ThrowCallBeforeStartedException();
	}

	void NZBDriveIMPL::SetConnectionInfoHandler(NewsClientCache::ConnectionInfoFunction handler)
	{
		if (m_state == Stopped)
		{
			m_clients.SetConnectionInfoHandler(RelayEvent(m_event_strand, handler));
		}
		else ThrowCallBeforeStartedException();
	}

	void NZBDriveIMPL::SetNZBFileOpenHandler(NZBFileOpenFunction handler)
	{
		if (m_state == Stopped)
		{
			m_nzbFileOpenFunction = RelayEvent(m_event_strand, handler);
		}
		else ThrowCallBeforeStartedException();
	}
	void NZBDriveIMPL::SetNZBFileCloseHandler(NZBFileCloseFunction handler)
	{
		if (m_state == Stopped)
		{
			m_nzbFileCloseFunction = RelayEvent(m_event_strand, handler);
		}
		else ThrowCallBeforeStartedException();
	}
	void NZBDriveIMPL::SetFileAddedHandler(NZBFileAddedFunction handler)
	{
		if (m_state == Stopped)
		{
			m_fileAddedFunction = RelayEvent(m_event_strand, handler);
		}
		else ThrowCallBeforeStartedException();
	}
	void NZBDriveIMPL::SetFileInfoHandler(NZBFileInfoFunction handler)
	{
		if (m_state == Stopped)
		{
			m_fileInfoFunction = RelayEvent(m_event_strand, handler);
		}
		else ThrowCallBeforeStartedException();
	}
	void NZBDriveIMPL::SetFileSegmentStateChangedHandler(NZBFileSegmentStateChangedFunction handler)
	{
		if (m_state == Stopped)
		{
			m_fileSegmentStateChangedFunction = RelayEvent(m_event_strand, handler);
		}
		else ThrowCallBeforeStartedException();
	}
	void NZBDriveIMPL::SetFileRemovedHandler(NZBFileRemovedFunction handler)
	{
		if (m_state == Stopped)
		{
			m_fileRemovedFunction = RelayEvent(m_event_strand, handler);
		}
		else ThrowCallBeforeStartedException();
	}

	void NZBDriveIMPL::SetLoggingLevel(const LogLineLevel level)
	{
		m_logger.SetLevel(level);
	}

	void NZBDriveIMPL::SetCachePath(const boost::filesystem::path& cache_path)
	{
		m_cache_path = cache_path;
	}

	boost::filesystem::path NZBDriveIMPL::GetCachePath() const
	{
		return m_cache_path;
	}

	int32_t NZBDriveIMPL::AddServer(const UsenetServer& server)
	{
		NewsServer srv;
		srv.Servername = server.ServerName;
		srv.Username = server.UserName;
		srv.Password = server.Password;
		srv.Port = server.Port;
		srv.SSL = server.UseSSL;
		srv.Timeout = server.Timeout;
		srv.Pipeline = server.Pipeline;

		return m_clients.AddServer(srv,
			server.Priority == 0 ? NewsServerPriority::Primary : NewsServerPriority::Backup,
			server.Connections);
	}

	void NZBDriveIMPL::RemoveServer(const int32_t id)
	{
		m_clients.RemoveServer(id);
	}

	bool NZBDriveIMPL::Start()
	{
		std::unique_lock<std::mutex> lk(m_mutex);

		if (m_state == Started) return true;

		m_startTime = std::chrono::steady_clock::now();

		m_event_work.reset(new boost::asio::io_service::work(m_event_io_service));
		m_event_thread = std::thread([this](){ m_event_io_service.run(); });

		HeartBeat(boost::system::error_code());

		m_clients.SetWaitingQueueEmptyHandler(
			[this]()
		{
#ifdef WIN32			  
			std::shared_ptr<ReadAheadFile> readAheadFile = atomic_load(&m_readAheadFile);
#else
			std::shared_ptr<ReadAheadFile> readAheadFile = m_readAheadFile;
#endif

			if (readAheadFile) readAheadFile->BufferNextSegment();
			//				if (m_readAheadFile) m_readAheadFile->BufferNextSegment();
		}
		);


		m_work.reset(new boost::asio::io_service::work(m_io_service));
		m_thread = std::thread([this](){ m_io_service.run(); });

		m_state = Started;

		return true;
	}


	void NZBDriveIMPL::HeartBeat(const boost::system::error_code& e)
	{
		if (e != boost::asio::error::operation_aborted)
		{
			//			std::shared_ptr<NZBDrive> me = shared_from_this();

			std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
			int64_t nms = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_startTime).count();

			m_clients.SendConnectionInfoEvents(nms);

			m_timer.expires_from_now(m_duration);
			m_timer.async_wait(
				[this](const boost::system::error_code& e)
			{
				HeartBeat(e);
			}
			);
		}

	}
	void NZBDriveIMPL::Stop()
	{
		std::unique_lock<std::mutex> lk(m_mutex);

		if (m_state == Stopped) return;

		_Stop();

		m_io_service.stop();
		m_work.reset();
		m_thread.join();
	}

	void NZBDriveIMPL::_Stop()
	{
		while (!m_mountStates.empty()) // Cancel all unfinished mounters.
		{
			Unmount(m_mountStates.begin());
		}

		m_event_io_service.stop();

		m_limiter.Stop();
		m_clients.Stop();
		m_root_dir->Clear();
		m_root_rawdir->Clear();

		// Stop event-service:
		m_event_work.reset();
		m_event_thread.join();

		m_state = Stopped;

		ResetHandlers();
	}

	int32_t NZBDriveIMPL::Mount(const int32_t nzbID, const filesystem::path& expanded_cache_path, const filesystem::path& mountdir, const nzb& mountfile,
		MountStatusFunction mountStatusFunction)
	{
		auto drive_mounter = std::make_shared<NZBDriveMounter>(*this, nzbID, mountdir, m_log, mountStatusFunction);

		{
			std::unique_lock<std::mutex> lock(m_mutex);

			if (!m_mountStates.insert(std::pair<const boost::filesystem::path, std::shared_ptr<NZBDriveMounter>>(mountdir, drive_mounter)).second)
			{
				m_log << Logger::PopupError << "Mounting of " << mountdir << " failed. NZB file is already mounted." << Logger::End;
				return MountErrorCode::MountingFailed;
			}
		}

		SendEvent(m_nzbFileOpenFunction, nzbID, mountdir);

		for (const nzb::file& nzbfile : mountfile.files)
		{
			int32_t fileID = m_fileCount++;

			std::shared_ptr<NZBFile> file(
				new NZBFile(m_io_service, nzbID, fileID, nzbfile, m_clients, expanded_cache_path, m_log,
				m_fileAddedFunction, m_fileInfoFunction, m_fileSegmentStateChangedFunction, m_fileRemovedFunction)
				);

			drive_mounter->parts_total++;
			file->AsyncGetFilename(
				[file, drive_mounter, nzbID, mountdir, this](const boost::filesystem::path& filename)
			{
				if (0 != ((int)file->GetErrorFlags() & (int)FileErrorFlag::CacheFileError))
				{
					if (drive_mounter->state == NZBDriveMounter::Mounting)
					{
						m_log << Logger::PopupError << "Failed to allocate cache-file. Mounting of " << mountdir << " will be canceled.\nPlease free some disk-space and try again." << Logger::End;
						Unmount(mountdir);
					}
					return;
				}

				if (filename.empty())
				{
					for (int n = 1; true; ++n)
					{
						std::ostringstream oss;
						oss << "ErrorFile" << n;
						if (!m_root_dir->Exists(drive_mounter->mountdir / oss.str()))
						{
							m_log << Logger::Error << "Could not read filename, using filename " <<
								oss.str() << " instead" << Logger::End;
							drive_mounter->errors++;
							drive_mounter->StartInsertFile(file, drive_mounter->mountdir / oss.str());
							break;
						}
					}
				}
				else
				{
					try
					{
						m_root_rawdir->RegisterFile(file, drive_mounter->mountdir / filename);
					}
					catch (std::exception& e)
					{
						m_log << Logger::Error << "Error when registering file: " << e.what() << Logger::End;
						drive_mounter->errors++;
					}
					drive_mounter->StartInsertFile(file, drive_mounter->mountdir);
				}
				drive_mounter->StopInsertFile();
			},
				&drive_mounter->cancel
				);
		}

		return nzbID;
	}

	int32_t NZBDriveIMPL::Mount(const filesystem::path& mountdir, const filesystem::path& nzbfile, MountStatusFunction mountStatusFunction)
	{
		if (m_state != Started) return MountErrorCode::MountingFailed;

		int32_t nzbID = m_nzbCount++;


		boost::filesystem::path expanded_cache_path;

		expanded_cache_path = m_cache_path;

		if (expanded_cache_path.empty())
		{
			m_log << ByteFountain::Logger::PopupError << "Invalid cache path" << ByteFountain::Logger::End;
			return MountErrorCode::InvalidCacheDir;
		}
		if (!expand_user(expanded_cache_path))
		{
			m_log << ByteFountain::Logger::PopupError << "Invalid cache path" << ByteFountain::Logger::End;
			return MountErrorCode::InvalidCacheDir;
		}
		if (!boost::filesystem::is_directory(expanded_cache_path))
		{
			bool ok = false;
			try
			{
				ok = boost::filesystem::create_directory(expanded_cache_path);
			}
			catch (const boost::filesystem::filesystem_error& e)
			{
				m_log << ByteFountain::Logger::PopupError << "Cannot create cache path: " << expanded_cache_path << ByteFountain::Logger::End;
				m_log << ByteFountain::Logger::Debug << e.what() << ByteFountain::Logger::End;
			}
			if (!ok)
			{
				return MountErrorCode::InvalidCacheDir;
			}
		}

		std::string err_msg;

		nzb mountfile = loadnzb(nzbfile, err_msg);

		if (err_msg.size()>0)
		{
			m_log << Logger::PopupError << err_msg << Logger::End;
			return MountErrorCode::LoadNZBFileError;
		}

		if (!Validate(mountfile))
		{
			m_log << Logger::PopupError << "File is not a valid nzb-file: " << nzbfile << Logger::End;
			return MountErrorCode::InvalidNZBFileError;
		}

		boost::uintmax_t nzbbytes = Bytes(mountfile);

		boost::filesystem::space_info sinfo = boost::filesystem::space(expanded_cache_path);

		if (nzbbytes>sinfo.available)
		{
			m_log << Logger::PopupError << "Not enough space in cache directory to mount: " << nzbfile << Logger::End;
			return MountErrorCode::CacheDirFullError;
		}

		return Mount(nzbID, expanded_cache_path, mountdir, mountfile, mountStatusFunction);
	}

	NZBDriveIMPL::MountStates::iterator NZBDriveIMPL::Unmount(const MountStates::iterator& it_state)
	{
		if (it_state != m_mountStates.end())
		{
			std::shared_ptr<NZBDriveMounter>& mounter = it_state->second;

			if (it_state->second->state == NZBDriveMounter::Mounting)
			{
				m_log << Logger::Info << "Canceling mount in progress..." << Logger::End;
				mounter->Cancel();
				m_log << Logger::Info << "Canceling done." << Logger::End;
			}

			m_root_dir->Unmount(mounter->mountdir);
			m_root_rawdir->Unmount(mounter->mountdir);

#ifdef WIN32
			std::atomic_store(&m_readAheadFile, std::shared_ptr<ReadAheadFile>(nullptr)); // In case we have unmounte the read-ahead-file
#else
			m_readAheadFile = std::shared_ptr<ReadAheadFile>(nullptr);
#endif
			m_log << Logger::Debug << "Unmount done " << mounter->mountdir << Logger::End;

			SendEvent(m_nzbFileCloseFunction, mounter->nzbID);

			return m_mountStates.erase(it_state);
		}
		return m_mountStates.end();
	}

	int32_t NZBDriveIMPL::Unmount(const filesystem::path& mountdir)
	{
		m_log << Logger::Info << "Unmounting: " << mountdir << Logger::End;

		std::unique_lock<std::mutex> lock(m_mutex);

		MountStates::iterator itMountState = m_mountStates.find(mountdir);

		if (itMountState != m_mountStates.end())
		{
			int32_t h = itMountState->second->nzbID;
			Unmount(itMountState);
#ifdef WIN32
			std::atomic_store(&m_readAheadFile, std::shared_ptr<ReadAheadFile>(nullptr)); // In case we have unmounte the read-ahead-file
#else
			m_readAheadFile = std::shared_ptr<ReadAheadFile>(nullptr);
#endif
			return h;
		}

		return MountErrorCode::UnmountingFailed;
	}
	std::shared_ptr<IDirectory> NZBDriveIMPL::GetRootDir()
	{
		return m_root_dir;
	}

	std::shared_ptr<IDirectory> NZBDriveIMPL::GetDirectory(const filesystem::path& p)
	{
		return m_root_dir->GetDirectory(p);
	}

	std::shared_ptr<IFile> NZBDriveIMPL::GetFile(const filesystem::path& p)
	{
		return m_root_dir->GetFile(p);
	}

	std::vector< std::pair<filesystem::path, ContentType> > NZBDriveIMPL::GetContent()
	{
		return m_root_dir->GetContent();
	}

	void NZBDriveIMPL::EnumFiles(std::function<void(const filesystem::path& path, std::shared_ptr<IFile> file)> callback)
	{
		m_root_dir->EnumFiles(callback);
	}

	uint_fast64_t NZBDriveIMPL::GetTotalNumberOfBytes() const
	{
		return m_root_dir->GetTotalNumberOfBytes();
	}




}