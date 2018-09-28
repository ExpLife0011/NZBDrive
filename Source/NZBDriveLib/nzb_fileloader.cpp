/*
    Copyright (c) 2017, Ole Stauning
    All rights reserved.

    Use of this source code is governed by a GPLv3-style license that can be found
    in the LICENSE file.
*/

#include "nzb.hpp"
#include "nzb_fileloader.hpp"
#include "text_tool.hpp"
#include <tinyxml2.h>
#include <boost/lexical_cast.hpp>
#include <boost/filesystem.hpp>


namespace ByteFountain
{
	using namespace tinyxml2;
//	using namespace boost::network;

	nzb loadnzb(const std::string& nzbfile, XMLDocument& doc, std::string& err_msg)
	{

		XMLHandle hDoc(&doc);
		XMLElement* pElem;
		XMLHandle hRoot(0);

		// block: nzb
		{
			pElem=hDoc.FirstChildElement().ToElement();
			// should always have a valid root but handle gracefully if it does
			if (!pElem || pElem->Value()!=std::string("nzb") )
			{
				std::ostringstream oss;
				oss<<"Invalid nzb file: "<<nzbfile<<std::endl;
				err_msg=oss.str();
				return nzb();
			}
			// save this for later
			hRoot=XMLHandle(pElem);
		}

		ByteFountain::nzb nzb;

		XMLElement* pElemFile=hRoot.FirstChildElement( "file" ).ToElement();
		if (!pElemFile)
		{
			std::ostringstream oss;
			oss<<"Invalid nzb file (no file-tag): "<<nzbfile<<std::endl;
			err_msg=oss.str();
			return nzb;
		}
		size_t nfiles=0;
		for(/*nothing*/; pElemFile; pElemFile=pElemFile->NextSiblingElement()) ++nfiles;

		pElemFile=hRoot.FirstChildElement( "file" ).ToElement();
		nzb.files.resize(nfiles);
		size_t i=0;
		for(/*nothing*/; pElemFile; pElemFile=pElemFile->NextSiblingElement())
		{
			ByteFountain::nzb::file& file=nzb.files[i++];
			const char* val;
			val=pElemFile->Attribute("poster");
			if (val!=0) file.poster=ByteFountain::text_tool::utf8_to_unicode(std::string(val));
			val=pElemFile->Attribute("subject");
			if (val!=0) file.subject=ByteFountain::text_tool::utf8_to_unicode(std::string(val));
			val=pElemFile->Attribute("date");
			if (val!=0) file.date=boost::lexical_cast<long>(val);

			XMLElement* pElemGroup=XMLHandle(pElemFile).FirstChildElement( "groups" ).FirstChildElement( "group" ).ToElement();
			if (!pElemGroup)
			{
				std::ostringstream oss;
				oss<<"Invalid nzb file (no group-tag): "<<nzbfile<<std::endl;
				err_msg=oss.str();
				return nzb;
			}
			for(/*nothing*/; pElemGroup; pElemGroup=pElemGroup->NextSiblingElement())
			{
				file.groups.insert(pElemGroup->GetText());
			}

			XMLElement* pElemSegment=XMLHandle(pElemFile).FirstChildElement( "segments" ).FirstChildElement( "segment" ).ToElement();
			if (!pElemSegment)
			{
				std::ostringstream oss;
				oss<<"Invalid nzb file (no segment-tag): "<<nzbfile<<std::endl;
				err_msg=oss.str();
				return nzb;
			}
			for(/*nothing*/; pElemSegment; pElemSegment=pElemSegment->NextSiblingElement())
			{
				ByteFountain::nzb::file::segment segment;
				segment.message_id=pElemSegment->GetText();
				val=pElemSegment->Attribute("bytes");
				if (val!=0) segment.bytes=boost::lexical_cast<unsigned long>(val);
				val=pElemSegment->Attribute("number");
				if (val!=0) segment.number=boost::lexical_cast<long>(val);

				file.segments.push_back(segment);
			}
		}

		return nzb;
	}

	nzb loadnzb_from_file(const std::string& location, const boost::filesystem::path& nzbfile, std::string& err_msg)
	{
		err_msg.clear();
#ifdef _MSC_VER
		FILE* file=0;
		if (0!=_wfopen_s(&file,nzbfile.c_str(), L"rb"))
		{
			std::ostringstream oss;
			oss<<"Could not find file: "<<nzbfile<<std::endl;
			err_msg=oss.str();
			return nzb();
		}
#else
		FILE* file = fopen(nzbfile.string().c_str(), "rb");
#endif
		XMLDocument doc;
		auto xmlerr = doc.LoadFile(file);
		if (xmlerr != XMLError::XML_SUCCESS)
		{
			std::ostringstream oss;
			oss<<"Could not load nzb file: "<<nzbfile<<", error is: "<<doc.ErrorStr()<<std::endl;
			err_msg=oss.str();
			return nzb();
		}
		return loadnzb(location,doc,err_msg);
	}
/*
	nzb loadnzb_from_uri(const std::string& location, const uri::uri& nzburi, std::string& err_msg)
	{
		try
		{
			http::client client;
			http::client::request request(nzburi);
			http::client::response response = client.get(request);

			std::string strdoc(static_cast<std::string>(body(response)));
			XMLDocument doc;
			auto xmlerr = doc.Parse(strdoc.c_str());
			if (xmlerr != XMLError::XML_SUCCESS)
			{
				std::ostringstream oss;
				oss<<"Could not parse file at : "<<location<<", error is: "<<doc.GetErrorStr1()<<std::endl;
				err_msg=oss.str();

				return nzb();
			}

			return loadnzb(location,doc,err_msg);
		}
		catch (std::exception &e) 
		{
			err_msg = e.what();
		}
	}
*/

	nzb loadnzb(const std::string& location, std::string& err_msg)
	{
//		uri::uri nzburi(location);

//		if (nzburi.is_valid())
//		{
//			return loadnzb_from_uri(location,nzburi,err_msg);
//		}

		boost::filesystem::path nzbfile(location);
		return loadnzb_from_file(location,nzbfile,err_msg);
	}
}



