/*

Copyright (c) 2010, Stephane Magnenat, ASL, ETHZ, Switzerland
You can contact the author at <stephane at magnenat dot net>

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL ETH-ASL BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#ifdef HAVE_OPENCL

#include "nabo_private.h"
#include "index_heap.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <stdexcept>
#include <limits>
#include <queue>
#include <algorithm>
#include <map>
#include <boost/numeric/conversion/bounds.hpp>
#include <boost/limits.hpp>
#include <boost/format.hpp>
#include <boost/thread.hpp>


/*!	\file kdtree_opencl.cpp
 \ *brief kd-tree search, opencl implementation
 \ingroup private
 */

namespace cl
{
	typedef std::vector<Device> Devices;
}

namespace Nabo
{
	//! \ingroup private
	//@{
	
	//! Maximum number of points acceptable in a query
	#define MAX_K 32
	
	using namespace std;
	
	template<typename T>
	struct EnableCLTypeSupport {};
	
	template<> struct EnableCLTypeSupport<float>
	{
		static string code(const cl::Device& device)
		{
			return "typedef float T;\n";
		}
	};
	
	template<> struct EnableCLTypeSupport<double>
	{
		static string code(const cl::Device& device)
		{
			string s;
			const string& exts(device.getInfo<CL_DEVICE_EXTENSIONS>());
			//cerr << "extensions: " << exts << endl;
			// first try generic 64-bits fp, otherwise try to fall back on vendor-specific extensions
			if (exts.find("cl_khr_fp64") != string::npos)
				s += "#pragma OPENCL EXTENSION cl_khr_fp64 : enable\n";
			else if (exts.find("cl_amd_fp64") != string::npos)
				s += "#pragma OPENCL EXTENSION cl_amd_fp64 : enable\n";
			else
				throw runtime_error("The OpenCL platform does not support 64 bits double-precision floating-points scalars.");
			s += "typedef double T;\n";
			return s;
		}
	};
	
	struct SourceCacher
	{
		typedef std::vector<cl::Device> Devices;
		typedef std::map<std::string, cl::Program> ProgramCache;
		
		cl::Context context;
		Devices devices;
		ProgramCache cachedPrograms;
		
		SourceCacher(const cl_device_type deviceType)
		{
			// looking for platforms, AMD drivers do not like the default for creating context
			vector<cl::Platform> platforms;
			cl::Platform::get(&platforms);
			if (platforms.empty())
				throw runtime_error("No OpenCL platform found");
			//for(vector<cl::Platform>::iterator i = platforms.begin(); i != platforms.end(); ++i)
			//	cerr << "platform " << i - platforms.begin() << " is " << (*i).getInfo<CL_PLATFORM_VENDOR>() << endl;
			cl::Platform platform = platforms[0];
			const char *userDefinedPlatform(getenv("NABO_OPENCL_USE_PLATFORM"));
			if (userDefinedPlatform)
			{
				size_t userDefinedPlatformId = atoi(userDefinedPlatform);
				if (userDefinedPlatformId < platforms.size())
					platform = platforms[userDefinedPlatformId];
			}
			
			// create OpenCL contexts
			cl_context_properties properties[] = { CL_CONTEXT_PLATFORM, (cl_context_properties)platform(), 0 };
			bool deviceFound = false;
			try {
				context = cl::Context(deviceType, properties);
				deviceFound = true;
			} catch (cl::Error e) {
				cerr << "Cannot find device type " << deviceType << " for OpenCL, falling back to any device" << endl;
			}
			if (!deviceFound)
				context = cl::Context(CL_DEVICE_TYPE_ALL, properties);
			devices = context.getInfo<CL_CONTEXT_DEVICES>();
			if (devices.empty())
				throw runtime_error("No devices on OpenCL platform");
		}
		
		~SourceCacher()
		{
			cerr << "Destroying source cacher containing " << cachedPrograms.size() << " cached programs" << endl;
		}
		
		bool contains(const std::string& source)
		{
			return cachedPrograms.find(source) != cachedPrograms.end();
		}
	};
	
	class ContextManager
	{
	public:
		typedef std::map<cl_device_type, SourceCacher*> Devices;
		
		ContextManager() {}
		~ContextManager()
		{
			cerr << "Destroying CL context manager, used " << devices.size() << " contexts" << endl;
			for (Devices::iterator it(devices.begin()); it != devices.end(); ++it)
				delete it->second;
		}
		cl::Context& createContext(const cl_device_type deviceType)
		{
			boost::mutex::scoped_lock lock(mutex);
			Devices::iterator it(devices.find(deviceType));
			if (it == devices.end())
			{
				it = devices.insert(
					pair<cl_device_type, SourceCacher*>(deviceType, new SourceCacher(deviceType))
					).first;
			}
			return it->second->context;
		}
		SourceCacher* getSourceCacher(const cl_device_type deviceType)
		{
			boost::mutex::scoped_lock lock(mutex);
			Devices::iterator it(devices.find(deviceType));
			if (it == devices.end())
				throw runtime_error("Attempt to get source cacher before creating a context");
			return it->second;
		}
		
	protected:
		Devices devices;
		boost::mutex mutex;
	};
	
	static ContextManager contextManager;
	
	template<typename T>
	OpenCLSearch<T>::OpenCLSearch(const Matrix& cloud, const Index dim, const unsigned creationOptionFlags, const cl_device_type deviceType):
		NearestNeighbourSearch<T>::NearestNeighbourSearch(cloud, dim, creationOptionFlags),
		deviceType(deviceType),
		context(contextManager.createContext(deviceType))
	{
	}
	
	template<typename T>
	void OpenCLSearch<T>::initOpenCL(const char* clFileName, const char* kernelName, const string& additionalDefines)
	{
		const bool collectStatistics(creationOptionFlags & NearestNeighbourSearch<T>::TOUCH_STATISTICS);
		
		SourceCacher* sourceCacher(contextManager.getSourceCacher(deviceType));
		SourceCacher::Devices& devices(sourceCacher->devices);
		
		// build and load source files
		cl::Program::Sources sources;
		// build defines
		ostringstream oss;
		oss << EnableCLTypeSupport<T>::code(devices.back());
		oss << "#define EPSILON " << numeric_limits<T>::epsilon() << "\n";
		oss << "#define DIM_COUNT " << dim << "\n";
		//oss << "#define CLOUD_POINT_COUNT " << cloud.cols() << "\n";
		oss << "#define POINT_STRIDE " << cloud.stride() << "\n";
		oss << "#define MAX_K " << MAX_K << "\n";
		if (collectStatistics)
			oss << "#define TOUCH_STATISTICS\n";
		oss << additionalDefines;
		//cerr << "params:\n" << oss.str() << endl;
		
		const std::string& source(oss.str());
		if (!sourceCacher->contains(source))
		{
			const size_t defLen(source.length());
			char *defContent(new char[defLen+1]);
			strcpy(defContent, source.c_str());
			sources.push_back(std::make_pair(defContent, defLen));
			string sourceFileName(OPENCL_SOURCE_DIR);
			sourceFileName += clFileName;
			// load files
			const char* files[] = {
				OPENCL_SOURCE_DIR "structure.cl",
				OPENCL_SOURCE_DIR "heap.cl",
				sourceFileName.c_str(),
				NULL 
			};
			for (const char** file = files; *file != NULL; ++file)
			{
				std::ifstream stream(*file);
				if (!stream.good())
					throw runtime_error((string("cannot open file: ") + *file));
				
				stream.seekg(0, std::ios_base::end);
				size_t size(stream.tellg());
				stream.seekg(0, std::ios_base::beg);
				
				char* content(new char[size + 1]);
				std::copy(std::istreambuf_iterator<char>(stream),
							std::istreambuf_iterator<char>(), content);
				content[size] = '\0';
				
				sources.push_back(std::make_pair(content, size));
			}
			sourceCacher->cachedPrograms[source] = cl::Program(context, sources);
			cl::Program& program = sourceCacher->cachedPrograms[source];
			
			// build
			cl::Error error(CL_SUCCESS);
			try {
				program.build(devices);
			} catch (cl::Error e) {
				error = e;
			}
			
			// dump
			for (cl::Devices::const_iterator it = devices.begin(); it != devices.end(); ++it)
			{
				cerr << "device : " << it->getInfo<CL_DEVICE_NAME>() << "\n";
				cerr << "compilation log:\n" << program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(*it) << endl;
			}
			// cleanup sources
			for (cl::Program::Sources::iterator it = sources.begin(); it != sources.end(); ++it)
			{
				delete[] it->first;
			}
			sources.clear();
			
			// make sure to stop if compilation failed
			if (error.err() != CL_SUCCESS)
				throw error;
		}
		cl::Program& program = sourceCacher->cachedPrograms[source];
		
		// build kernel and command queue
		knnKernel = cl::Kernel(program, kernelName); 
		queue = cl::CommandQueue(context, devices.back());
		
		// map cloud
		if (!(cloud.Flags & Eigen::DirectAccessBit) || (cloud.Flags & Eigen::RowMajorBit))
			throw runtime_error("wrong memory mapping in point cloud");
		const size_t cloudCLSize(cloud.cols() * cloud.stride() * sizeof(T));
		cloudCL = cl::Buffer(context, CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR, cloudCLSize, const_cast<T*>(&cloud.coeff(0,0)));
		knnKernel.setArg(0, sizeof(cl_mem), &cloudCL);
	}
	
	template<typename T>
	unsigned long OpenCLSearch<T>::knn(const Matrix& query, IndexMatrix& indices, Matrix& dists2, const Index k, const T epsilon, const unsigned optionFlags)
	{
		checkSizesKnn(query, indices, dists2, k);
		const bool collectStatistics(creationOptionFlags & NearestNeighbourSearch<T>::TOUCH_STATISTICS);
		
		// check K
		if (k > MAX_K)
			throw runtime_error("number of neighbors too large for OpenCL");
		
		// check consistency of query wrt cloud
		if (query.stride() != cloud.stride() ||
			query.rows() != cloud.rows())
			throw runtime_error("query is not of the same dimensionality as the point cloud");
		
		// map query
		if (!(query.Flags & Eigen::DirectAccessBit) || (query.Flags & Eigen::RowMajorBit))
			throw runtime_error("wrong memory mapping in query data");
		const size_t queryCLSize(query.cols() * query.stride() * sizeof(T));
		cl::Buffer queryCL(context, CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR, queryCLSize, const_cast<T*>(&query.coeff(0,0)));
		knnKernel.setArg(1, sizeof(cl_mem), &queryCL);
		// map indices
		assert((indices.Flags & Eigen::DirectAccessBit) && (!(indices.Flags & Eigen::RowMajorBit)));
		const int indexStride(indices.stride());
		const size_t indicesCLSize(indices.cols() * indexStride * sizeof(int));
		cl::Buffer indicesCL(context, CL_MEM_WRITE_ONLY | CL_MEM_USE_HOST_PTR, indicesCLSize, &indices.coeffRef(0,0));
		knnKernel.setArg(2, sizeof(cl_mem), &indicesCL);
		// map dists2
		assert((dists2.Flags & Eigen::DirectAccessBit) && (!(dists2.Flags & Eigen::RowMajorBit)));
		const int dists2Stride(dists2.stride());
		const size_t dists2CLSize(dists2.cols() * dists2Stride * sizeof(T));
		cl::Buffer dists2CL(context, CL_MEM_WRITE_ONLY | CL_MEM_USE_HOST_PTR, dists2CLSize, &dists2.coeffRef(0,0));
		knnKernel.setArg(3, sizeof(cl_mem), &dists2CL);
		
		// set resulting parameters
		knnKernel.setArg(4, k);
		knnKernel.setArg(5, 1 + epsilon);
		knnKernel.setArg(6, optionFlags);
		knnKernel.setArg(7, indexStride);
		knnKernel.setArg(8, dists2Stride);
		knnKernel.setArg(9, cloud.cols());
		
		// if required, map visit count
		vector<cl_uint> visitCounts;
		const size_t visitCountCLSize(query.cols() * sizeof(cl_uint));
		cl::Buffer visitCountCL;
		if (collectStatistics)
		{
			visitCounts.resize(query.cols());
			visitCountCL = cl::Buffer(context, CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR, visitCountCLSize, &visitCounts[0]);
			knnKernel.setArg(10, sizeof(cl_mem), &visitCountCL);
		}
		
		// execute query
		queue.enqueueNDRangeKernel(knnKernel, cl::NullRange, cl::NDRange(query.cols()), cl::NullRange);
		queue.enqueueMapBuffer(indicesCL, true, CL_MAP_READ, 0, indicesCLSize, 0, 0);
		queue.enqueueMapBuffer(dists2CL, true, CL_MAP_READ, 0, dists2CLSize, 0, 0);
		if (collectStatistics)
			queue.enqueueMapBuffer(visitCountCL, true, CL_MAP_READ, 0, visitCountCLSize, 0, 0);
		queue.finish();
		
		// if required, collect statistics
		if (collectStatistics)
		{
			unsigned long totalVisitCounts(0);
			for (size_t i = 0; i < visitCounts.size(); ++i)
				totalVisitCounts += (unsigned long)visitCounts[i];
			return totalVisitCounts;
		}
		else
			return 0;
	}
	
	template<typename T>
	BruteForceSearchOpenCL<T>::BruteForceSearchOpenCL(const Matrix& cloud, const Index dim, const unsigned creationOptionFlags, const cl_device_type deviceType):
	OpenCLSearch<T>::OpenCLSearch(cloud, dim, creationOptionFlags, deviceType)
	{
		// compute bounds
		for (int i = 0; i < cloud.cols(); ++i)
		{
			const Vector& v(cloud.block(0,i,this->dim,1));
			const_cast<Vector&>(this->minBound) = this->minBound.cwise().min(v);
			const_cast<Vector&>(this->maxBound) = this->maxBound.cwise().max(v);
		}
		
		// init openCL
		initOpenCL("knn_bf.cl", "knnBruteForce");
	}
	
	template struct BruteForceSearchOpenCL<float>;
	template struct BruteForceSearchOpenCL<double>;
	
	
	template<typename T>
	size_t argMax(const typename NearestNeighbourSearch<T>::Vector& v)
	{
		T maxVal(0);
		size_t maxIdx(0);
		for (int i = 0; i < v.size(); ++i)
		{
			if (v[i] > maxVal)
			{
				maxVal = v[i];
				maxIdx = i;
			}
		}
		return maxIdx;
	}

	template<typename T>
	size_t KDTreeBalancedPtInLeavesStackOpenCL<T>::getTreeSize(size_t elCount) const
	{
		// FIXME: 64 bits safe stuff, only work for 2^32 elements right now
		assert(elCount > 0);
		elCount --;
		size_t count = 0;
		int i = 31;
		for (; i >= 0; --i)
		{
			if (elCount & (1 << i))
				break;
		}
		for (int j = 0; j <= i; ++j)
			count |= (1 << j);
		count <<= 1;
		count |= 1;
		return count;
	}
	
	template<typename T>
	size_t KDTreeBalancedPtInLeavesStackOpenCL<T>::getTreeDepth(size_t elCount) const
	{
		if (elCount <= 1)
			return 0;
		elCount --;
		size_t i = 31;
		for (; i >= 0; --i)
		{
			if (elCount & (1 << i))
				break;
		}
		return i+1;
	}

	template<typename T>
	void KDTreeBalancedPtInLeavesStackOpenCL<T>::buildNodes(const BuildPointsIt first, const BuildPointsIt last, const size_t pos, const Vector minValues, const Vector maxValues)
	{
		const size_t count(last - first);
		//cerr << count << endl;
		if (count == 1)
		{
			const int d = -2-(first->index);
			assert(pos < nodes.size());
			nodes[pos] = Node(d);
			return;
		}
		
		// find the largest dimension of the box
		size_t cutDim = argMax<T>(maxValues - minValues);
		
		// compute number of elements
		const size_t rightCount(count/2);
		const size_t leftCount(count - rightCount);
		assert(last - rightCount == first + leftCount);
		
		// sort
		nth_element(first, first + leftCount, last, CompareDim(cutDim));
		
		// set node
		const T cutVal((first+leftCount)->pos.coeff(cutDim));
		nodes[pos] = Node(cutDim, cutVal);
		
		//cerr << pos << " cutting on " << cutDim << " at " << (first+leftCount)->pos[cutDim] << endl;
		
		// update bounds for left
		Vector leftMaxValues(maxValues);
		leftMaxValues[cutDim] = cutVal;
		// update bounds for right
		Vector rightMinValues(minValues);
		rightMinValues[cutDim] = cutVal;
		
		// recurse
		buildNodes(first, first + leftCount, childLeft(pos), minValues, leftMaxValues);
		buildNodes(first + leftCount, last, childRight(pos), rightMinValues, maxValues);
	}
	
	template<typename T>
	KDTreeBalancedPtInLeavesStackOpenCL<T>::KDTreeBalancedPtInLeavesStackOpenCL(const Matrix& cloud, const Index dim, const unsigned creationOptionFlags, const cl_device_type deviceType):
		OpenCLSearch<T>::OpenCLSearch(cloud, dim, creationOptionFlags, deviceType)
	{
		// build point vector and compute bounds
		BuildPoints buildPoints;
		buildPoints.reserve(cloud.cols());
		for (int i = 0; i < cloud.cols(); ++i)
		{
			const Vector& v(cloud.block(0,i,this->dim,1));
			buildPoints.push_back(BuildPoint(v, i));
			const_cast<Vector&>(minBound) = minBound.cwise().min(v);
			const_cast<Vector&>(maxBound) = maxBound.cwise().max(v);
		}
		
		// create nodes
		nodes.resize(getTreeSize(cloud.cols()));
		buildNodes(buildPoints.begin(), buildPoints.end(), 0, minBound, maxBound);
		const unsigned maxStackDepth(getTreeDepth(nodes.size()) + 1);
		
		// init openCL
		initOpenCL("knn_kdtree_pt_in_leaves.cl", "knnKDTree", (boost::format("#define MAX_STACK_DEPTH %1%\n") % maxStackDepth).str());
		
		// map nodes, for info about alignment, see sect 6.1.5 
		const size_t nodesCLSize(nodes.size() * sizeof(Node));
		nodesCL = cl::Buffer(context, CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR, nodesCLSize, &nodes[0]);
		knnKernel.setArg(10, sizeof(cl_mem), &nodesCL);
	}

	template struct KDTreeBalancedPtInLeavesStackOpenCL<float>;
	template struct KDTreeBalancedPtInLeavesStackOpenCL<double>;
	
	
	template<typename T>
	size_t KDTreeBalancedPtInNodesStackOpenCL<T>::getTreeSize(size_t elCount) const
	{
		// FIXME: 64 bits safe stuff, only work for 2^32 elements right now
		size_t count = 0;
		int i = 31;
		for (; i >= 0; --i)
		{
			if (elCount & (1 << i))
				break;
		}
		for (int j = 0; j <= i; ++j)
			count |= (1 << j);
		//cerr << "tree size " << count << " (" << elCount << " elements)\n";
		return count;
	}
	
	template<typename T>
	size_t KDTreeBalancedPtInNodesStackOpenCL<T>::getTreeDepth(size_t elCount) const
	{
		// FIXME: 64 bits safe stuff, only work for 2^32 elements right now
		int i = 31;
		for (; i >= 0; --i)
		{
			if (elCount & (1 << i))
				break;
		}
		return i + 1;
	}
	
	template<typename T>
	void KDTreeBalancedPtInNodesStackOpenCL<T>::buildNodes(const BuildPointsIt first, const BuildPointsIt last, const size_t pos, const Vector minValues, const Vector maxValues)
	{
		const size_t count(last - first);
		//cerr << count << endl;
		if (count == 1)
		{
			nodes[pos] = Node(-1, *first);
			return;
		}
		
		// find the largest dimension of the box
		const size_t cutDim = argMax<T>(maxValues - minValues);
		
		// compute number of elements
		const size_t recurseCount(count-1);
		const size_t rightCount(recurseCount/2);
		const size_t leftCount(recurseCount-rightCount);
		assert(last - rightCount == first + leftCount + 1);
		
		// sort
		nth_element(first, first + leftCount, last, CompareDim(cloud, cutDim));
		
		// set node
		const Index index(*(first+leftCount));
		const T cutVal(cloud.coeff(cutDim, index));
		nodes[pos] = Node(cutDim, index);
		
		//cerr << pos << " cutting on " << cutDim << " at " << (first+leftCount)->pos[cutDim] << endl;
		
		// update bounds for left
		Vector leftMaxValues(maxValues);
		leftMaxValues[cutDim] = cutVal;
		// update bounds for right
		Vector rightMinValues(minValues);
		rightMinValues[cutDim] = cutVal;
		
		// recurse
		if (count > 2)
		{
			buildNodes(first, first + leftCount, childLeft(pos), minValues, leftMaxValues);
			buildNodes(first + leftCount + 1, last, childRight(pos), rightMinValues, maxValues);
		}
		else
		{
			nodes[childLeft(pos)] = Node(-1, *first);
			nodes[childRight(pos)] = Node(-2, 0);
		}
	}
	
	template<typename T>
	KDTreeBalancedPtInNodesStackOpenCL<T>::KDTreeBalancedPtInNodesStackOpenCL(const Matrix& cloud, const Index dim, const unsigned creationOptionFlags, const cl_device_type deviceType):
	OpenCLSearch<T>::OpenCLSearch(cloud, dim, creationOptionFlags, deviceType)
	{
		const bool collectStatistics(creationOptionFlags & NearestNeighbourSearch<T>::TOUCH_STATISTICS);
		
		// build point vector and compute bounds
		BuildPoints buildPoints;
		buildPoints.reserve(cloud.cols());
		for (int i = 0; i < cloud.cols(); ++i)
		{
			buildPoints.push_back(i);
			const Vector& v(cloud.block(0,i,this->dim,1));
			const_cast<Vector&>(minBound) = minBound.cwise().min(v);
			const_cast<Vector&>(maxBound) = maxBound.cwise().max(v);
		}
		
		// create nodes
		nodes.resize(getTreeSize(cloud.cols()));
		buildNodes(buildPoints.begin(), buildPoints.end(), 0, minBound, maxBound);
		const unsigned maxStackDepth(getTreeDepth(nodes.size()) + 1);
		
		// init openCL
		initOpenCL("knn_kdtree_pt_in_nodes.cl", "knnKDTree", (boost::format("#define MAX_STACK_DEPTH %1%\n") % maxStackDepth).str());
		
		// map nodes, for info about alignment, see sect 6.1.5 
		const size_t nodesCLSize(nodes.size() * sizeof(Node));
		nodesCL = cl::Buffer(context, CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR, nodesCLSize, &nodes[0]);
		if (collectStatistics)
			knnKernel.setArg(11, sizeof(cl_mem), &nodesCL);
		else
			knnKernel.setArg(10, sizeof(cl_mem), &nodesCL);
	}
	
	template struct KDTreeBalancedPtInNodesStackOpenCL<float>;
	template struct KDTreeBalancedPtInNodesStackOpenCL<double>;
	
	//@}
}

#endif // HAVE_OPENCL