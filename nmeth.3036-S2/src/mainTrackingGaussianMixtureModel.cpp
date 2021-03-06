/*
 * mainTrackingGaussianMixtureModel.cpp
 */



#include <string>
#include <ctime>
#include <list>
#include <iostream>
#include <fstream>
#include <stdio.h>
#include <utility>
#include <time.h>
#include <map>
#include "GaussianMixtureModel.h"
#include "responsibilities.h"
#include "variationalInference.h"
#include "external/xmlParser2/xmlParser.h"
#include "Utils/parseConfigFile.h"
#include "external/Nathan/tictoc.h"
#include "sys/stat.h"
#include "UtilsCUDA/knnCuda.h"
#include "UtilsCUDA/GMEMupdateCUDA.h"
#include "constants.h"
#include "temporalLogicalRules/temporalLogicalRules.h"
#include "UtilsCUDA/3DEllipticalHaarFeatures/EllipticalHaarFeatures.h"
#include "UtilsCUDA/3DEllipticalHaarFeatures/gentleBoost/gentleBoost.h"
#include "trackletCalculation.h"
#include "cellDivision.h"

#ifdef PICTOOLS_JP2K
#include "ioFunctions.h"
#endif

#include "hierarchicalSegmentation.h"
#include "supportFunctionsMain.h"

#include "backgroundDetectionInterface.h"

#if defined(_WIN32) || defined(_WIN64)
#define NOMINMAX
#include <Windows.h>
#include "Shlwapi.h"
#endif


#pragma comment(lib, "Shlwapi.lib")

namespace mylib
{
extern "C"
{
#include "mylib/mylib.h"
#include "mylib/array.h"
#include "mylib/image.h"
#include "mylib/histogram.h"
}

};


using namespace std;

//#define DEBUG_EM_ITER //uncomment this line to write XML files with all the steps within an iteration of the Variational inference EM
//#define DEBUG_TGMM_XML_FILES //uncomment this line to write xml files with intermediate steps during the sequential GMM fitting process

//TODO: WORK_WITH_UNITS only works with CUDA code so far
//#define WORK_WITH_UNITS //uncomment this define to work with metric units (instead of pixels). So scale is incoporated directly in all the calculation.


#ifndef ROUND
#define ROUND(x) (floor(x+0.5))
#endif


//-----------------------------------------------
//to separate filename from path
//from http://www.cplusplus.com/reference/string/string/find_last_of/
//returns the
string getPathFromFilename (const std::string& str)
{
  //std::cout << "Splitting: " << str << '\n';
  unsigned found = str.find_last_of("/\\");
  //std::cout << " path: " << str.substr(0,found) << '\n';
  //std::cout << " file: " << str.substr(found+1) << '\n';
  return str.substr(0,found);
}
//-------------------------------------------------
//structure to sort foreground values based on supervoxels identity
struct RegionId
{
    unsigned short int labelId;//id for the region
	long long int posId;//position in the original vector (so we can recover sort and apply it to other vectors)
	long long int idx;//position in the original image (so we can store super voxels)
    
	friend bool operator< ( RegionId& lhs, RegionId& rhs);
};

bool operator< (RegionId& lhs, RegionId& rhs)
{
    return lhs.labelId<rhs.labelId;
}

//------------------------------------------------
//structure to accomodate offsets in the data: if in imageJ offset is (x,y,z) -> the file contains (t,y,x,z) [Matlab style]
struct OffsetCoordinates
{
	float x,y,z;

	OffsetCoordinates() { x = y = z = 0;};
	OffsetCoordinates (const OffsetCoordinates& p)
	{
		x = p.x;
		y = p.y;
		z = p.z;
	}
	OffsetCoordinates& operator=(const OffsetCoordinates & p)
	{
		if (this != &p)
		{
			x = p.x;
			y = p.y;
			z = p.z;
		}
		return *this;
	}
};

//===========================================
int main( int argc, const char** argv )
{	
	//------------------------------parameters that should be set by user later on (Advanced mode)-----------------------
	bool regularize_W4DOF = true;//whether we allow rotation in Z for Gaussian covariance or not. If regularize_W4DOF==true->W_02 = W_12 = 0		
	

	
//from http://stackoverflow.com/questions/1023306/finding-current-executables-path-without-proc-self-exe
//for platform-dependant options
	string pathS;
	string folderSeparator("/");	
#if defined(_WIN32) || defined(_WIN64)
	char path[MAX_PATH];
	//find out folder of the executable
	HMODULE hModule = GetModuleHandleW(NULL);
	GetModuleFileName(hModule, path, MAX_PATH);
	//change to process stack
	PathRemoveFileSpec(path);	
	pathS = string(path);
	folderSeparator = string("\\");
#elif defined(__APPLE__)
	//find out folder of the executable
	cout<<"ERROR: this part of the code has not been ported to MacOSx yet!!!"<<endl;
	return 2;	
#else //we assume Linux
	//find out folder of the executable
	char buff[1024];
	string buffP;
    	ssize_t len = ::readlink("/proc/self/exe", buff, sizeof(buff)-1);
    	if (len != -1) {
      		buff[len] = '\0';
	      buffP = std::string(buff);
    	} else {
     		/* handle error condition */
    	}
	//change to process stack
	pathS = getPathFromFilename(buffP);	
#endif
	string backgroundClassifierFilename(pathS + folderSeparator + "classifierBackgroundTracks.txt");
	int temporalWindowSizeForBackground = 5;//size of the temporal window to calculate temporal features for background detection. This has to match the classifier!!!

	//--------------------------------parse input arguments from config file---------------------------
	if(argc!=4 && argc!=5)
	{
		cout<<"ERROR: wrong number of input arguments"<<endl;
		exit(2);
	}

	//set main parameters for the algorithm
	configOptionsTrackingGaussianMixture configOptions;
	string configFilename(argv[1]);
	if(configOptions.parseConfigFileTrackingGaussianMixture(configFilename)!=0) exit(2);

	regularizePrecisionMatrixConstants::setConstants(configOptions.lambdaMin, configOptions.lambdaMax, configOptions.maxExcentricity);

	//trim supervoxels
	int minNucleiSize = configOptions.minNucleiSize;//in voxels
	int maxNucleiSize = configOptions.maxNucleiSize;
	imgVoxelType tauMax = 300;//to trim hierarchical segmentation
	float maxPercentileTrimSV = configOptions.maxPercentileTrimSV;//percentile to trim supervoxel
	int conn3Dsv = configOptions.conn3DsvTrim;//connectivity to trim supervoxel

	//for logical temporal rules
	const int lengthTMthr = configOptions.SLD_lengthTMthr;//for delete short lived cell (minimum length of daughter before ending track)
	const int minNeighboringVoxels = 10;//if two supervoxels are have less than minNeighboringVoxels as common border using minNeighboringConn3D, then they must belong to a different nucleus
	const int minNeighboringVoxelsConn3D = 4;
	
	//for merge split
	const int mergeSplitDeltaZ = 11;
	int64 boundarySizeIsNeigh[dimsImage];
	int conn3DIsNeigh = 74;



	int devCUDA=selectCUDAdeviceMaxMemory();
	devCUDA=0;
	cout<<"Overwriting CUDA device selction to default"<<endl;
	if(devCUDA<0)
	{
		exit(2);
	}else{
		cout<<"Selected CUDA device "<<devCUDA<<endl;
	}


	//set init and end frame
	int iniFrame=0,endFrame=0;
	//parse input arguments
	iniFrame=atoi(argv[2]);
	endFrame=atoi(argv[3]);

	//read last entry to continue from that frame
	string debugPath;
	char extra[128];
	if(argc>=5)
	{
		debugPath=string(argv[4]);//we read previous xml file as initialization
		sprintf(extra,"%.4d",iniFrame-1);//we always read the same image. We could make the code faster by reading it only once but I jts want to test the idea
		string itoaD=string(extra);
		//configOptions.GMxmlIniFilename=string(debugPath+"XML_finalResult/GMEMfinalResult_frame"+ itoaD + ".xml");
		//cout<<"Starting from previous run. Make sure parameters are the same!!!. XMLini="<<configOptions.GMxmlIniFilename<<endl;
		cout<<"ERROR: Starting from previous run HAS NOT BEEN IMPLEMENTED YET"<<endl;
		return 3;
	}else{

#if defined(_WIN32) || defined(_WIN64)
		SYSTEMTIME str_t;
		GetSystemTime(&str_t);
		//sprintf(extra,"C:\\Users\\Fernando\\TrackingNuclei\\TGMMruns\\GMEMtracking3D_%d_%d_%d_%d_%d_%d\\",str_t.wYear,str_t.wMonth,str_t.wDay,str_t.wHour,str_t.wMinute,str_t.wSecond);
		sprintf(extra,"%s\\GMEMtracking3D_%d_%d_%d_%d_%d_%d\\",configOptions.debugPathPrefix.c_str(),str_t.wYear,str_t.wMonth,str_t.wDay,str_t.wHour,str_t.wMinute,str_t.wSecond);
#else
		sprintf(extra,"%s/GMEMtracking3D_%ld/",configOptions.debugPathPrefix.c_str(), time(NULL));
#endif
		debugPath=string(extra);
	}


	//---------------------------------end of parse input arguments--------------------------------------------


	//create debug folder
	struct stat St;
	int error;
	string cmd;

#if defined(_WIN32) || defined(_WIN64)
	if (GetFileAttributes(debugPath.c_str()) == INVALID_FILE_ATTRIBUTES)
	 {
		cmd=string("mkdir " + debugPath);
		error=system(cmd.c_str());
		if(error>0)
		{
			cout<<"ERROR ("<<error<<"): generating debug path "<<debugPath<<endl;
			cout<<"Wtih command "<<cmd<<endl;
			return error;
		}
	 }

#else
	if (stat( debugPath.c_str(), &St ) != 0)//check if folder exists
	{
		cmd=string("mkdir " + debugPath);
		error=system(cmd.c_str());
		if(error>0)
		{
			cout<<"ERROR ("<<error<<"): generating debug path "<<debugPath<<endl;
			cout<<"Wtih command "<<cmd<<endl;
			return error;
		}
	}
#endif


	//open file to record parameters
	char logBuffer[32];
	sprintf(logBuffer,"%.4d",iniFrame);//we always read the same image. We could make the code faster by reading it only once but I jts want to test the idea
	string itoaLog=string(logBuffer);
	ofstream outLog((debugPath + "experimentLog_"+ itoaLog + ".txt").c_str());
	outLog<<"Program called with the following command:"<<endl;
	for (int ii=0;ii<argc;ii++) outLog<<argv[ii]<<" ";
	outLog<<endl;
	outLog<<"minPi_kForDead="<<minPi_kForDead<<endl;
	outLog<<"maxGaussiansPerVoxel="<<maxGaussiansPerVoxel<<endl;
	configOptions.printConfigFileTrackingGaussianMixture(outLog);

	

	//creat elineage hypertree to store lineages
	lineageHyperTree lht(endFrame+1);
	float thrDist2LargeDisplacement = 2048.0f * 2048.0f;//to set low confidence level if a cell displaces more than this threshold (in pixels with scale). This is adapted at each time point from the data itself.


	//read file with offsets between time points
	vector< OffsetCoordinates > driftCorrectionVec;
	driftCorrectionVec.reserve( endFrame);
	if( strcmp(configOptions.offsetFile.c_str(),"empty") != 0 )//read file
	{
		ifstream fin( configOptions.offsetFile.c_str() );

		if( fin.is_open() == false )
		{
			cout<<"ERROR: reading offset file "<<configOptions.offsetFile<<endl;
			return 2;
		}else{
			cout<<"Reading offset file "<<configOptions.offsetFile<<endl;
		}

		string line;
		OffsetCoordinates auxOC;
		int tt;
		while ( getline (fin,line) )
		{
			std::istringstream(line) >> tt >> auxOC.x >> auxOC.y >> auxOC.z;
			driftCorrectionVec.resize( tt+1);
			driftCorrectionVec[tt] = auxOC;
		}

		fin.close();
	}


	//----------------------------------main for loop for sequential processing--------------------------
	char buffer[128];
	char buffer2[128];
	string itoa,itoa2;
	vector<GaussianMixtureModel*> vecGM;
	vecGM.reserve(10000);
	vector< pair<GaussianMixtureModel*,GaussianMixtureModel*> > splitSet;//stores candidates to be split
	splitSet.reserve(1000);
	vector<double> singleCellSplitScore;//store split scores without divisions to be able to compare afterwards
	singleCellSplitScore.reserve(1000);
	vector<GaussianMixtureModel> backupVecGM;
	backupVecGM.reserve(1000);
	vector<feature> Fx;
	vector<double> splitMergeTest;
	Fx.reserve(10000);
	splitMergeTest.reserve(10000);

	//load classifier
	string classifierCellDivisionFilename(pathS + folderSeparator +"classifierCellDivision.txt");

	vector< vector< treeStump> > classifierCellDivision;
	int errC = loadClassifier(classifierCellDivision, classifierCellDivisionFilename);
	if(errC > 0 ) return errC;
	long long int numFeatures = getNumberOfHaarFeaturesPerllipsoid();

	float scaleOrig[dimsImage];//to save original scale
	mylib::Dimn_Type imgSize[dimsImage];//to save size of image
	int imgDims=dimsImage;//to save number of dimensions
	for(int ii =0;ii<dimsImage-1;ii++)
		scaleOrig[ii] = 1.0f;
	scaleOrig[dimsImage-1] = configOptions.anisotropyZ;
	GaussianMixtureModel::setScale(scaleOrig);


	double numSamples;//cumulative sum of image intensity
	int numDiv=10;//number of divisions per frame

	//optical flow pointers
	mylib::Array* imgFlow=NULL;
	mylib::Array* imgFlowMask=NULL;//uint8 containing areas where we need flow estimation
	mylib::uint8* imgFlowMaskPtr=NULL;


	vector<hierarchicalSegmentation*> hsVec;//stores hierarchical segmentation for frames in the temporal window
	hsVec.reserve(10);//it shoudl match teh temporal window size

	for(int frame=iniFrame;frame<=endFrame;frame++)
	{
		TicTocTimer tt=tic();
		cout<<"Processing frame "<<frame<<endl;
		//==============================================================
		//read image
		sprintf(buffer,"%.5d",frame);
		itoa=string(buffer);
		sprintf(buffer,"%d",configOptions.tau);
		string itoaTau=string(buffer);
		//sprintf(buffer2,"%.3d",frame);
		//itoa2=string(buffer2);
		string imgBasename(configOptions.imgFilePattern );
		parseImagePath( imgBasename, frame );
		string imgFilename((imgBasename+ ".jp2").c_str());
		
		
		
		//to load hierarchical segmentation
		char bufferHS[128];
		sprintf(bufferHS, "_hierarchicalSegmentation_conn3D%d_medFilRad%d.bin",configOptions.conn3D, configOptions.radiusMedianFilter);
		string imgHS = string ((imgBasename + string(bufferHS)).c_str());
		
		

		//try to read JP2 image		
		mylib::Value_Type type;
		int ndims;
		mylib::Dimn_Type  *dims = NULL;
		void* data = NULL;
#ifdef PICTOOLS_JP2K
		ifstream fin(imgFilename.c_str());
		if( fin.is_open() == true )//jp2 file exists
		{
			fin.close();
			data = readJP2Kfile(imgFilename, type, ndims, &dims);				
		}
#endif
		mylib::Array *img = NULL;

		if(data == NULL)//try top read tif image
		{
			imgFilename = string((imgBasename + ".tif").c_str());
			img = mylib::Read_Image((char*)(imgFilename.c_str()),0);
			if( img == NULL)
			{
				cout<<"ERROR: reading image "<<imgFilename<<endl;
				exit(3);
			}
		}else{//wrap data into mylib::array
			img = mylib::Make_Array_Of_Data(mylib::PLAIN_KIND, type, ndims, dims, data);
		}


		//read hierarchical segmentation		
		ifstream inHS( imgHS.c_str(), ios::binary | ios::in );
		if( !inHS.is_open() )
		{
			cout<<"ERROR: could not open binary file "<<imgHS<<" to read hierarchical segmentation"<<endl;
			return 4;
		}
		hierarchicalSegmentation *hs = new hierarchicalSegmentation(inHS); //TODO: parse directly segmentation to supervoxels
		inHS.close();

		//hack to make the code work for uin8 without changing everything to templates
		//basically, parse the image to uint16, since the code was designed for uint16
		if( img->type == mylib::UINT8_TYPE )
		{	
			img = mylib::Convert_Array_Inplace (img, img->kind, mylib::UINT16_TYPE, 16, 0);
		}
		//hack to make the code work for 2D without changing everything to templates
		//basically, add one black slice to the image (you should select conn3D = 4 or 8)
		if( img->ndims == 2 )
		{	
			mylib::Dimn_Type dimsAux[dimsImage];
			for(int ii = 0; ii<img->ndims; ii++)
				dimsAux[ii] = img->dims[ii];
			for(int ii = img->ndims; ii<dimsImage; ii++)
				dimsAux[ii] = 2;

			mylib::Array *imgAux = mylib::Make_Array(img->kind, img->type, dimsImage, dimsAux);
			memset(imgAux->data,0, (imgAux->size) * sizeof(mylib::uint16) ); 
			memcpy(imgAux->data, img->data, img->size * sizeof(mylib::uint16) ); 

			mylib::Array* imgSwap = imgAux;
			img = imgAux;
			mylib::Free_Array( imgSwap);
		}
		if(img->ndims!=dimsImage)
		{
			cout<<"ERROR: constant dimsImage does not match img->ndims. Please change dimsImage and recompile the code"<<endl;
			exit(3);
		}
		//associate each supervoxel with the correct data pointer
		if(img->type != mylib::UINT16_TYPE )
		{
			cout<<"ERROR: code not ready for images that are not uint16"<<endl;
			return 7;
		}


		for(int ii = 0; ii < img->ndims; ii++)
		{
			if( img->dims[ii] != supervoxel::dataDims[ii] )
			{
				if( img->dims[0] == supervoxel::dataDims[1] && img->dims[1] == supervoxel::dataDims[0] && img->dims[2] == supervoxel::dataDims[2])
				{
					cout<<"WARNING:"<<supervoxel::dataDims[0]<<"x"<<supervoxel::dataDims[1]<<"x"<<supervoxel::dataDims[2]<<endl;
					cout<<"WARNING:"<<img->dims[0]<<"x"<<img->dims[1]<<"x"<<img->dims[2]<<endl;
					cout<<"WARNING: transposing image"<<endl;
					transposeStackUINT16( img );
					break;
				}else{
					
					cout<<"ERROR!: data dimensions do not match between image and supervoxel record. Most likely a flipped x and y due to tiff to jp2 format with Matlab"<<endl;
					cout<<"ERROR:"<<supervoxel::dataDims[0]<<"x"<<supervoxel::dataDims[1]<<"x"<<supervoxel::dataDims[2]<<endl;
					cout<<"ERROR:"<<img->dims[0]<<"x"<<img->dims[1]<<"x"<<img->dims[2]<<endl;
					exit(3);
				}
			}
		}

		supervoxel::dataSizeInBytes = sizeof(mylib::uint16) * img->size;
		supervoxel::setScale(GaussianMixtureModel::scale);
		for(unsigned int ii = 0;ii < hs->getNumberOfBasicRegions(); ii++)
		{
			hs->basicRegionsVec[ii].dataPtr = img->data;
			hs->basicRegionsVec[ii].TM = frame;
		}

		hsVec.push_back(hs);
		//setup trimming parameters
		supervoxel::setTrimParameters(maxNucleiSize, maxPercentileTrimSV, conn3Dsv);
		hs->setMaxTau( tauMax );

		//merge / split parameters for HS
		int64* neighOffsetIsNeigh = supervoxel::buildNeighboorhoodConnectivity(conn3DIsNeigh + 1, boundarySizeIsNeigh);//using the special neighborhood for coarse sampling
		supervoxel::pMergeSplit.setParam(conn3DIsNeigh, neighOffsetIsNeigh, mergeSplitDeltaZ);
		delete[] neighOffsetIsNeigh;


		
		//generate segmentation mask at specific tau
		cout<<"Generating segmentation mask from HS (trimming supervoxels)."<<endl;
		TicTocTimer ttHS = tic();				
		
		
		mylib::uint16* imgDataUINT16 = (mylib::uint16*) (img->data);
		hs->segmentationAtTau( configOptions.tau );		


		int64 boundarySize[dimsImage];//margin we need to calculate connected components	
		int64 *neighOffsetSv = supervoxel::buildNeighboorhoodConnectivity(conn3Dsv, boundarySize);
		mylib::uint16 thr;
		bool *imgVisited = new bool[img->size];
		memset(imgVisited, 0, sizeof(bool) * img->size );//if visited->true (so I can set teh rest to zero)

		//necessary to initiate reposnsibilities
		long long int query_nb=0;		

		vector<int> eraseIdx;
		eraseIdx.reserve( hs->currentSegmentatioSupervoxel.size() / 10 );
		int numSplits = 0;		
		for(size_t ii = 0; ii < hs->currentSegmentatioSupervoxel.size(); ii++)
		{
			thr = hs->currentSegmentatioSupervoxel[ii].trimSupervoxel<mylib::uint16>();//trimmming supervoxels			

			//split supervoxels if necessary (obvius oversegmentations)
			TreeNode<nodeHierarchicalSegmentation>* rootSplit[2];
			supervoxel rootSplitSv[2];
			float scoreS;
			queue<size_t> q;
			q.push(ii);
			while( q.empty() == false )
			{
				size_t aa = q.front();
				q.pop();			
				if( hs->currentSegmentatioSupervoxel[aa].getDeltaZ() > supervoxel::pMergeSplit.deltaZthr )
				{
					scoreS = hs->suggestSplit<mylib::uint16>(hs->currentSegmentationNodes[aa],hs->currentSegmentatioSupervoxel[aa], rootSplit, rootSplitSv);
					if( scoreS > 0.0f )
					{
						hs->currentSegmentatioSupervoxel[aa] = rootSplitSv[0];//root split has the correct nodeHSptr
						hs->currentSegmentatioSupervoxel.push_back( rootSplitSv[1] );						
						hs->currentSegmentationNodes[aa] = rootSplit[0];
						hs->currentSegmentationNodes.push_back( rootSplit[1] );

						numSplits++;
						q.push(aa);
						q.push(hs->currentSegmentationNodes.size() - 1 );
					}
					//recalculate thr
					thr = numeric_limits<mylib::uint16>::max();
					for(vector<uint64>::const_iterator iter = hs->currentSegmentatioSupervoxel[ii].PixelIdxList.begin(); iter != hs->currentSegmentatioSupervoxel[ii].PixelIdxList.end(); ++iter)
					{
						thr = min(thr , imgDataUINT16[ *iter ] );
					}
				}
			}

			
			if( hs->currentSegmentatioSupervoxel[ii].PixelIdxList.size() < minNucleiSize )//delete supervoxel
			{
				eraseIdx.push_back(ii);
			}else{//local background subtraction and reference points for local geometrical descriptors
				hs->currentSegmentatioSupervoxel[ii].weightedGaussianStatistics<mylib::uint16>(true);

				localGeometricDescriptor<dimsImage>::addRefPt(frame, hs->currentSegmentatioSupervoxel[ii].centroid);

				for(vector<uint64>::const_iterator iter = hs->currentSegmentatioSupervoxel[ii].PixelIdxList.begin(); iter != hs->currentSegmentatioSupervoxel[ii].PixelIdxList.end(); ++iter)
				{
					//local background subtraction: since these are the trimmed supervoxels, they are guaranteed to be above thr
					imgDataUINT16[ *iter ] -= thr;
					imgVisited[ *iter ] = true;
				}
			}

			query_nb += hs->currentSegmentatioSupervoxel[ii].PixelIdxList.size();
			hs->currentSegmentatioSupervoxel[ii].localBackgroundSubtracted = true;

		}
		//set to true for all basic regions
		for(unsigned int ii = 0; ii < hs->getNumberOfBasicRegions(); ii++)
			hs->basicRegionsVec[ii].localBackgroundSubtracted = true;

		//delete supervoxels that are too small		
		if( eraseIdx.size() >= hs->currentSegmentatioSupervoxel.size() )
		{
			cout<<"ERROR: we are going to delete all the supervoxels!!!"<<endl;
			exit(5);
		}
		size_t auxSize = hs->currentSegmentatioSupervoxel.size();
		hs->eraseSupervoxelFromCurrentSegmentation(eraseIdx);
		cout<<"Deleted "<<eraseIdx.size()<<" supervoxels out of "<<auxSize<<" for being of size<"<<minNucleiSize<<" after trimming. Left "<<hs->currentSegmentatioSupervoxel.size()<<endl;
		cout<<"Splitted "<<numSplits<<" supervoxels for having deltaZ >" <<supervoxel::pMergeSplit.deltaZthr<<endl;							


		//set all the non-visited elements to zero
		for(mylib::Size_Type ii = 0; ii< img->size; ii++)
		{
			if( imgVisited[ii] == false )
			{
				imgDataUINT16[ii] = 0;
			}
		}

		delete[] neighOffsetSv;
		delete[] imgVisited;
		cout<<"Parsing "<<hs->currentSegmentatioSupervoxel.size()<<" supervoxels from HS took "<<toc(&ttHS)<<" secs"<<endl;


		//save image dimensions
		for(int aa=0;aa<img->ndims;aa++) imgSize[aa]=img->dims[aa];
		imgDims = img->ndims;


		//copy image before converting it to float (we need it for image features in the GPU)
		mylib::Array* imgUINT16 = mylib::Make_Array(img->kind, img->type, img->ndims,img->dims);
		mylib::uint16* imgUINT16ptr = (mylib::uint16*) (imgUINT16->data);
		memcpy(imgUINT16->data, img->data, sizeof(mylib::uint16) * (img->size) );

		mylib::Convert_Array_Inplace(img, mylib::PLAIN_KIND, mylib::FLOAT32_TYPE, 1,1.0);
		mylib::Value v1a,v0a;
		v1a.fval=1.0;v0a.fval=0.0;		
		mylib::Scale_Array_To_Range(img,v0a,v1a);
		

		//TODO make it work for any type of data
		mylib::float32 *imgData=(mylib::float32*)(img->data);
		
		if(img->type!=8)
		{
			cout<<"ERROR: code is only ready for FLOAT32 images"<<endl;
			exit(10);
		}		

		//update supervoxel informations: img->data pointter has changed after rescaling
		supervoxel::dataSizeInBytes = sizeof(float) * img->size;
		for(unsigned int ii = 0;ii < hs->getNumberOfBasicRegions(); ii++)
		{
			hs->basicRegionsVec[ii].dataPtr = img->data;
		}

		for(unsigned int ii = 0;ii < hs->currentSegmentatioSupervoxel.size(); ii++)
		{
			hs->currentSegmentatioSupervoxel[ii].dataPtr = img->data;
		}



		//---------------------------------------------------------------
		//estimate optical flow
		if(configOptions.estimateOpticalFlow==2)//calculate flow on the fly
		{
			//parameters for optical are set inside that routine. They should be the same for all datasets
			cout<<"Calculating optical flow. MAKE SURE ALL PARAMETERS WERE SET APPROPIATELY FOR THIS DATASET."<<endl;
			
			
			string filenameInTarget( configOptions.imgFilePattern );
			parseImagePath(filenameInTarget, frame - 1 );
			string filenameInSource( configOptions.imgFilePattern );
			parseImagePath(filenameInSource, frame );

			
			string filenameInTargetMask( filenameInTarget + "backgroundPredictionIlastik_" );//using Ilastik predictions

			//int auxMask=(int)(configOptions.thrSignal*255.0);
			int auxMask = 1;//current code uses supervoxels (no more Ilastik background)
			if(auxMask<0 || auxMask>255)
			{
				cout<<"ERROR: thrSignal ooutside bounds"<<endl;
				exit(2);
			}
			mylib::uint8 maskThr=(mylib::uint8)(auxMask);

			int err=0;
			if(frame-1<0)//initial frame: we can not calculate flow
			{
				mylib::Dimn_Type* ndims=new mylib::Dimn_Type[dimsImage+1];
				for(int ii=0;ii<dimsImage;ii++) ndims[ii]=img->dims[ii];
				ndims[dimsImage]=dimsImage;
				imgFlow=mylib::Make_Array(mylib::PLAIN_KIND,mylib::FLOAT32_TYPE,dimsImage+1,ndims);
				memset(imgFlow->data,0,imgFlow->size*sizeof(mylib::float32));
				delete[] ndims;
			}
			else
				//err=opticalFlow_anisotropy5(filenameInSource,filenameInTarget,filenameInTargetMask,maskThr,imgFlow,configOptions.maxDistPartitionNeigh);
				cout<<"WARNING: Option not available in the current code. You need to implement your own optical flow code (or can request ours via email) and paste the function call here"<<endl;
			if(err>0) 
				return err;
		}else if(configOptions.estimateOpticalFlow==1){//load precalculated flow

			char bufferDmax[64];
			sprintf(bufferDmax,"%d",int(configOptions.maxDistPartitionNeigh));
			string itoaDmax(bufferDmax);

			string filenameFlow((imgBasename+ "flowArray_dMax" + itoaDmax + "_" + itoa + ".bin").c_str());
			ifstream inFlow(filenameFlow.c_str(),ios::binary | ios::in);
			if(!inFlow.is_open())
			{
				cout<<"ERROR: opening flow array file : "<<filenameFlow<<endl;
				return 2;
			}else{
				cout<<"Using file "<<filenameFlow<<" for flow information"<<endl;
			}
			mylib::Dimn_Type* ndims=new mylib::Dimn_Type[dimsImage+1];
			for(int ii=0;ii<dimsImage;ii++) ndims[ii]=img->dims[ii];
			ndims[dimsImage]=dimsImage;
			imgFlow=mylib::Make_Array(mylib::PLAIN_KIND,mylib::FLOAT32_TYPE,dimsImage+1,ndims);
			inFlow.read((char*)(imgFlow->data),imgFlow->size);
			inFlow.close();
			delete[] ndims;
		}

		//-------------------------------------------------------------

		//------------------------------------------
		//estimate threshold of what is signal and what is not
		
		//cout<<"WARNING: we assume images have been treated to remove non-uniform background since we are fitting a Gaussian Mixture model (background should be zero value)"<<endl;//TODO in C++


		//initialize responsibilities
		unsigned short int numLabels = hs->currentSegmentatioSupervoxel.size();//keeps track of the maximum number of labels that are assigned
		setDeviceCUDA(devCUDA);
		//host temporary memory needed
		float *queryHOST=new float[query_nb*dimsImage];
		float *imgDataHOST=new float[query_nb];
		float *centroidLabelPositionHOST=new float[numLabels*dimsImage];//stores the centroid for each supervoxel
		float *centroidLabelWeightHOST=new float[numLabels];//stores the wieght (based on intensity) for each supervoxel
		long long int *labelListPtrHOST=new long long int[numLabels+1];//same concept as column-compressed sparse matrices. When we sort positions by region id (i.e. supervoxels), then all the elements for i-th region are bteween labelListPtr[ii]<=p<labelListPtr[ii+1]  ii=0,...,numLabels-1. Thus, labelsListPtr[0]=0 and labelaListPtr[numLabels]=query_nb

		
		memset(centroidLabelPositionHOST,0,sizeof(float)*numLabels*dimsImage);//reset
		memset(centroidLabelWeightHOST,0,sizeof(float)*numLabels);
		


		//initialize CUDA arrays to hold 3D points that represent signal
		float* queryCUDA=NULL;//holds 3D locations of each signal pixel in order to find k-NN
		float* imgDataCUDA=NULL;//holds intensity of each pixel
		float *rnkCUDA=NULL;
		float *rnkCUDAtr=NULL;//not really needed anymore
		int *indCUDA=NULL;
		int *indCUDAtr=NULL;
		float *centroidLabelPositionCUDA=NULL;
		long long int *labelListPtrCUDA=NULL;

		long long int idxQuery=0;
		mylib::Coordinate *coord;
		mylib::Dimn_Type *ccAux;
		numSamples=0.0;

		long long int offset=0,offsetL=0;
		unsigned short int auxL=0;


		//main loop to parse information from HS to lineageHyperTree and vecGM: remember supervoxels have been trimmed already
		list<supervoxel> *listPtr = (&(lht.supervoxelsList[frame]));//to avoid refering to it all the time
		listPtr->clear();

		labelListPtrHOST[0]=0;
		for(mylib::uint16 ii = 0; ii < hs->currentSegmentatioSupervoxel.size(); ii++)
		{
			for(vector<uint64>::const_iterator iter = hs->currentSegmentatioSupervoxel[ii].PixelIdxList.begin(); iter != hs->currentSegmentatioSupervoxel[ii].PixelIdxList.end(); ++iter)
			{
				imgDataHOST[idxQuery] = imgData[*iter];
				//find closest maxGaussiansPerVoxel Gaussians
				coord=mylib::Idx2CoordA(img,*iter);
				ccAux=(mylib::Dimn_Type*)coord->data;
				offset = idxQuery;
				for(int ii=0;ii<dimsImage;ii++)
				{
					#ifdef WORK_WITH_UNITS
					queryHOSTaux[offset]=scaleOrig[ii]*ccAux[ii];//transform index 2 coordinates
#else
					queryHOST[offset] = (float)ccAux[ii];//transform index 2 coordinates
#endif
					offset += query_nb;//interleave memory to favor coalescenc access in GPU
				}
				idxQuery++;
				mylib::Free_Array(coord);
			}

			//set arrays related to each label
			centroidLabelWeightHOST[ii] = hs->currentSegmentatioSupervoxel[ii].intensity;
			labelListPtrHOST[ ii + 1 ] = labelListPtrHOST[ ii ] + hs->currentSegmentatioSupervoxel[ii].PixelIdxList.size();
			offsetL = ii;
			for(int jj=0;jj<dimsImage;jj++)
			{
				
				centroidLabelPositionHOST[offsetL] = hs->currentSegmentatioSupervoxel[ii].centroid[jj];
				offsetL+=numLabels;
			}

			//update lineageHyperTree supervoxel (this is duplication needed for "backwards" compatibility)
			listPtr->push_back( hs->currentSegmentatioSupervoxel[ii] );
		}

		


		cout<<"Calculating nearest neighbors for supervoxels"<<endl;
		TicTocTimer ttKNN = tic();
		int err;
		if(frame > 0)
		{
			err = lht.supervoxelNearestNeighborsInTimeForward(frame-1, configOptions.KmaxNumNNsupervoxel,configOptions.KmaxDistKNNsupervoxel , devCUDA);
			if(err>0) return err;
		}
		err = lht.supervoxelNearestNeighborsInTimeBackward(frame, configOptions.KmaxNumNNsupervoxel,configOptions.KmaxDistKNNsupervoxel , devCUDA);
		if(err>0) return err;
		err = lht.supervoxelNearestNeighborsInSpace(frame, configOptions.KmaxNumNNsupervoxel,configOptions.KmaxDistKNNsupervoxel , devCUDA);
		if(err>0) return err;		

		cout<<"Nearest neighbors took "<<toc(&ttKNN)<<" secs"<<endl;
		//-----------------------------------------------------------------------
		

		//--------------------------------------------------------------
		//initialize Gaussians with priors from previous frame
		int numDeaths=0;
		int Wsize = dimsImage * dimsImage;
		if(frame==iniFrame)//generate initial frame from supervoxel segmentation
		{
			//generate one nuclei per supervoxel
			int countS = 0;
			ParentTypeSupervoxel iterN;
			list<lineage>::iterator iterL;
			for(list<supervoxel>::iterator iterS = lht.supervoxelsList[frame].begin(); iterS != lht.supervoxelsList[frame].end(); ++iterS, ++countS)
			{
				 iterN = lht.addNucleusFromSupervoxel(frame, iterS);//returns pointer to the newly created supervoxel
				 //we do not need to create new lineages: the program will create them automatically after the TGMM iteration since parentId = -1;
			}
			//generate vecGM from lht
			parseNucleiList2TGMM<float>(vecGM,lht,frame, true, thrDist2LargeDisplacement);
			for(unsigned int kk=0;kk<vecGM.size();kk++)//set alpha_o to some default value since many of them can die due to oversegmentation
			{
				vecGM[kk]->alpha_o = vecGM[kk]->N_k * 0.1;
				vecGM[kk]->parentId = -1;//so we know it is the beginning of a new lineage
			}			
			//update priors based on estimations: since this is the first frame it should be a very loose prior
			for(unsigned int kk=0;kk<vecGM.size();kk++)
			{
				vecGM[kk]->updatePriors(0.1,0.1,0.1, -1.0);//use alphaTotal=-1 to not update alpha_o	
				//hack to make it similar to previous code without sliding window
				vecGM[kk]->nu_k *= 2.0;
				for(int aa = 0; aa<Wsize; aa++) 
					vecGM[kk]->W_k.data()[aa] /= 2.0;
			}
			//remove nuclei from lht since the final result will be added after GMM
			for(list<supervoxel>::iterator iterS = lht.supervoxelsList[frame].begin(); iterS != lht.supervoxelsList[frame].end(); ++iterS, ++countS)
			{
				iterS->treeNode.deleteParent();
			}
			lht.nucleiList[frame].clear();

		}else//this is not the first time point
		{

			//generate vecGM from lht
			parseNucleiList2TGMM<float>(vecGM,lht,frame-1, true, thrDist2LargeDisplacement);//we generate nuclei list from previous frame-> we want to extend this solution to t+1 using TGMM framework
			double alphaTotal=0.0;
			for(unsigned int kk=0;kk<vecGM.size();kk++)
			{
				alphaTotal+=vecGM[kk]->alpha_k;
			}
			//cout<<imgFlow<<" "<<imgFlowMask<<endl;
			if(imgFlow!=NULL)
			{
				//TODO: I should use r_nk (responsibities) to calculate a weighted mean of the flow for each cell
				int err=applyFlowPredictionToNuclei(imgFlow,imgFlowMask,vecGM,true);//we use forward flow
				if(err>0) exit(err);
			}
			//update priors based on estimations
			for(unsigned int kk=0;kk<vecGM.size();kk++)
			{
				vecGM[kk]->updatePriors(configOptions.betaPercentageOfN_k,configOptions.nuPercentageOfN_k, configOptions.alphaPercentage, alphaTotal);//use alphaTotal=-1 to not update alpha_o
				//hack to make it similar to previous code without sliding window
				vecGM[kk]->nu_k *= 2.0;
				for(int aa = 0; aa<Wsize; aa++) 
					vecGM[kk]->W_k.data()[aa] /= 2.0;
			}
		}
		if( argc>=5 )
		{
			cout<<"ERROR: with new code using sliding window the option of starting after a crash is not coded yet"<<endl;
			exit(3);
		}

		//=====================================================
		//apply offset from txt file
		if( driftCorrectionVec.size() > frame )
		{
			for(unsigned int kk=0;kk<vecGM.size();kk++)
			{
				vecGM[kk]->m_k[0] += driftCorrectionVec[frame].x;
				vecGM[kk]->m_k[1] += driftCorrectionVec[frame].y;
				vecGM[kk]->m_k[2] += driftCorrectionVec[frame].z;

				vecGM[kk]->m_o[0] += driftCorrectionVec[frame].x;
				vecGM[kk]->m_o[1] += driftCorrectionVec[frame].y;
				vecGM[kk]->m_o[2] += driftCorrectionVec[frame].z;
			}
		}

		
		//--------------------------------------------------
		//---------------debug:write out iteration
		ofstream outXML;
		char buffer[128];

#ifdef DEBUG_TGMM_XML_FILES
		sprintf(buffer,"%.4d",frame);
		itoa=string(buffer);

		if (stat( (debugPath+"XML_KalmanFilterPrediction").c_str(), &St ) != 0)//check if folder exists
		{
			cmd=string("mkdir " + debugPath + "XML_KalmanFilterPrediction");
			error=system(cmd.c_str());
		}

		outXML.open((debugPath + "XML_KalmanFilterPrediction/" + "GMEMiniKalmanFilterPrediction_frame" + itoa + ".xml").c_str());
		GaussianMixtureModel::writeXMLheader(outXML);
		for(unsigned int ii=0;ii<vecGM.size();ii++)
		{
#ifdef WORK_WITH_UNITS
			vecGM[ii]->units2pixels(scaleOrig);
			vecGM[ii]->writeXML(outXML);
			vecGM[ii]->pixels2units(scaleOrig);
#else
			vecGM[ii]->writeXML(outXML);
#endif
		}
		GaussianMixtureModel::writeXMLfooter(outXML);
		outXML.close();
#endif
		//-----------------------------------------------------




#ifdef DEBUG_EM_ITER
		int frameToPrint=40000;//print out all the frames
		stringstream itoaFrame;
		itoaFrame<<frame;
		if(frame==frameToPrint || frameToPrint<0)
		{
			//--------------------------------------------------
			//debug:write out initial positions
			if (stat( (debugPath + "XML_GMEMiterations_frame" + itoaFrame.str() ).c_str(), &St ) != 0)//check if folder exists
					{
						cmd=string("mkdir " + debugPath + "XML_GMEMiterations_frame"+ itoaFrame.str());
						error=system(cmd.c_str());
					}

			outXML.open((debugPath + "XML_GMEMiterations_frame"+ itoaFrame.str() +"/debugEMGM_iter0000.xml").c_str());
			GaussianMixtureModel::writeXMLheader(outXML);
			for(unsigned int ii=0;ii<vecGM.size();ii++)
			{
#ifdef WORK_WITH_UNITS
			vecGM[ii]->units2pixels(scaleOrig);
			vecGM[ii]->writeXML(outXML);
			vecGM[ii]->pixels2units(scaleOrig);
#else
			vecGM[ii]->writeXML(outXML);
#endif
			}
			GaussianMixtureModel::writeXMLfooter(outXML);
			outXML.close();
			//-----------------------------------------------------
		}
#endif

	//----------------------------------------------------------------------------------------------------------


		//allocate memory in device
		//GMEMinitializeMemory(&queryCUDA,queryHOST,&imgDataCUDA,imgDataHOST,GaussianMixtureModel::scale,query_nb,&rnkCUDA,&indCUDA,&indCUDAtr);
		GMEMinitializeMemoryWithSupervoxels(&queryCUDA,queryHOST,&imgDataCUDA,imgDataHOST,GaussianMixtureModel::scale,query_nb,&rnkCUDA,&indCUDA,&indCUDAtr,numLabels,&centroidLabelPositionCUDA,centroidLabelPositionHOST,&labelListPtrCUDA,labelListPtrHOST);
		copyScaleToDvice(GaussianMixtureModel::scale);

		//-----------------------------------------------------------------

		//==============================================================

		//string debugPathCUDAa(debugPath+"XML_GMEMiterations_CUDA_noSplit_frame");		
		//string debugPathCUDAa("");

		//----------------to write rnk---------------------------------
		string debugPathCUDAa("");
#ifdef DEBUG_TGMM_XML_FILES
		sprintf(buffer,"%.4d",frame);//to save rnk and ind for each supervoxel
		itoa=string(buffer);
		debugPathCUDAa = string(debugPath+"XML_finalFirstRoundEMnoSplit");
		if (stat( debugPathCUDAa.c_str(), &St ) != 0)//check if folder exists
		{
			cmd=string("mkdir " + debugPathCUDAa);
			error=system(cmd.c_str());
		}
		debugPathCUDAa=string(debugPathCUDAa+ "/rnk_frame"+ itoa + ".bin");
#endif
		//----------------------------------------------------

		GaussianMixtureModelCUDA *vecGMHOST=new GaussianMixtureModelCUDA[vecGM.size()];
		for(unsigned int ii=0;ii<vecGM.size();ii++) 
			copy2GMEM_CUDA(vecGM[ii],&(vecGMHOST[ii]));
		//GMEMvariationalInferenceCUDA(queryCUDA,imgDataCUDA,rnkCUDA,rnkCUDAtr,indCUDA,indCUDAtr,vecGMHOST,query_nb,vecGM.size(),configOptions.maxIterEM,configOptions.tolLikelihood,devCUDA,frame, true, debugPathCUDAa);
		GMEMvariationalInferenceCUDAWithSupervoxels(queryCUDA,imgDataCUDA,rnkCUDA,rnkCUDAtr,indCUDA,indCUDAtr,centroidLabelPositionCUDA,labelListPtrCUDA,vecGMHOST,query_nb,vecGM.size(),numLabels,configOptions.maxIterEM,configOptions.tolLikelihood,devCUDA,frame, regularize_W4DOF, debugPathCUDAa);
		for(unsigned int ii=0;ii<vecGM.size();ii++) 
			copyFromGMEM_CUDA(&(vecGMHOST[ii]),vecGM[ii]);

		size_t vecGM_ref_nb = vecGM.size();//I need to save it for later
		//delete[] vecGMHOST; //we need it to estimate local likelihood before split


		//----------------debug--------------------------------------------
		string GMxmlFilename;
		sprintf(buffer,"%.4d",frame);
		itoa=string(buffer);

#ifdef DEBUG_TGMM_XML_FILES
		if (stat( (debugPath+"XML_finalFirstRoundEMnoSplit").c_str(), &St ) != 0)//check if folder exists
		{
			cmd=string("mkdir " + debugPath + "XML_finalFirstRoundEMnoSplit");
			error=system(cmd.c_str());
			if(error>0)
			{
				cout<<"ERROR: generating debug folder "<<cmd<<endl;
				cout<<"With command "<<cmd<<endl;
				return error;
			}
		}
		GMxmlFilename=string(debugPath+"XML_finalFirstRoundEMnoSplit/GMEMfinalFirstRoundEMnoSplit_frame"+ itoa + ".xml");
		string xmlFilenameBeforeSplit=GMxmlFilename;
		outXML.open(GMxmlFilename.c_str());
		GaussianMixtureModel::writeXMLheader(outXML);
		for(unsigned int ii=0;ii<vecGM.size();ii++)
		{
#ifdef WORK_WITH_UNITS
			vecGM[ii]->units2pixels(scaleOrig);
			vecGM[ii]->writeXML(outXML);
			vecGM[ii]->pixels2units(scaleOrig);
#else
			vecGM[ii]->writeXML(outXML);
#endif
		}
		GaussianMixtureModel::writeXMLfooter(outXML);
		outXML.close();
#endif

#ifndef CELL_DIVISION_WITH_GPU_3DHAAR //to comment out cell division using GPU 3D Haar features + likelihood ratio score 
		delete[] vecGMHOST; //we need it to estimate local likelihood before split
#else 

		//----------- check cell divisions using image features (GPU) and machine learning classifier----------------
		TicTocTimer ttCellDivision = tic();
		cout<<"Checking which ellipsoids out of "<<vecGM.size()<< " might be dividing with trained classifier"<<endl;
		Fx.resize(vecGM.size());
		//allocate memory and copy values
		long long int *dimsVec = new long long int[img->ndims];
		for(int aa = 0; aa < img->ndims; aa++)
			dimsVec[aa] = img->dims[aa];
		int numEllipsoids = vecGM.size();
		int sizeW = dimsImage * (1 + dimsImage) / 2;
		double *m = new double[dimsImage * numEllipsoids];
		double *W = new double[sizeW  * numEllipsoids];
		for(int jj = 0; jj<numEllipsoids; jj++)
		{
			int countW = jj;
			int countM = jj;
			for(int aa = 0; aa<dimsImage; aa++)
			{
				m[countM] = vecGM[jj]->m_k(aa);
				for( int bb = aa; bb < dimsImage; bb++)
				{
					W[ countW ] = vecGM[jj]->W_k(aa,bb) * vecGM[jj]->nu_k;
					countW += numEllipsoids;
				}
				countM += numEllipsoids;
			}			
		}
		//cout<<"Allocating memory took "<<toc(&ttCellDivision)<<" secs"<<endl;

		//calculate features 
		basicEllipticalHaarFeatureVector **fBasic = calculateEllipticalHaarFeatures(m, W,vecGM.size(),imgUINT16ptr,dimsVec,devCUDA,0);
		if(fBasic == NULL)
			return 1;//some error happened

		//cout<<"Calculating basic features took "<<toc(&ttCellDivision)<<" secs"<<endl;
		//extend Haar features
		int numHaarFeaturesPerEllipsoid = 0;
		float* xTest = NULL;
		calculateCombinationsOfBasicHaarFeatures(fBasic,vecGM.size(),&numHaarFeaturesPerEllipsoid, &xTest);
		if(xTest == NULL)
			return 2;//some error happened
		if( numHaarFeaturesPerEllipsoid != numFeatures )
		{
			cout<<"ERROR: numFeatures "<<numFeatures<<" is different than numHaarFeaturesPerEllipsoid "<<numHaarFeaturesPerEllipsoid<<endl;
			return 3;
		}
		//cout<<"Calculating extended features took "<<toc(&ttCellDivision)<<" secs"<<endl;
		//calculate classifier results
		//transposeXtrainOutOfPlace(xTest, Fx.size(), numFeatures);//we need to perform transposition from GPU features to gentleBoost classifier				
		boostingTreeClassifierTranspose(xTest,&(Fx[0]) ,classifierCellDivision , Fx.size(), numFeatures);

		if(vecGM.size()!=Fx.size())
		{
			cout<<"ERROR: after calling Matlab C Shared library for split classifier: vecGM and Fx are not the same size!"<<endl;
			exit(3);
		}
		for(unsigned int ii=0;ii<vecGM.size();ii++) vecGM[ii]->splitScore=Fx[ii];

		//release memory
		delete[] m;
		delete[] W;
		delete[] dimsVec;
		for(int ii=0;ii<vecGM.size();ii++) 
			delete fBasic[ii];
		delete[] fBasic;
		delete[] xTest;
		
		cout<<"Running classifer took "<<toc(&ttCellDivision)<<" secs"<<endl;
		//--------------------------end of cell division detection with image features + machine learning -----------------------------------------------


		//generate set S containing all candidates
		unsigned int sizeGMvec=vecGM.size();
		splitSet.clear();
		GaussianMixtureModel* GM;
		singleCellSplitScore.clear();
		backupVecGM.clear();
		vector<int> dividingNucleiIdx;//stores idx so we know which ones to compute local likelihood
		dividingNucleiIdx.reserve(vecGM.size() / 10);
		for(unsigned int kk=0;kk<sizeGMvec;kk++)
		{
			if(vecGM[kk]->splitScore>configOptions.thrSplitScore && vecGM[kk]->isDead()==false)//cell is a candidates to split
			{
				dividingNucleiIdx.push_back(kk);
				backupVecGM.push_back(*(vecGM[kk]));
				singleCellSplitScore.push_back(vecGM[kk]->splitScore);

				//generate split
				//update Gaussian mixture
					vecGM.push_back(new GaussianMixtureModel());
					vecGM[kk]->splitGaussian(vecGM.back(),vecGM.size()-1,imgData,img->dims);//using k-means
					GM=vecGM.back();//returns pointer of the new Gaussian

				splitSet.push_back(make_pair(vecGM[kk],GM));

				//update priors
				GM->updatePriorsAfterSplitProposal();
				vecGM[kk]->updatePriorsAfterSplitProposal();
			}else{
				vecGM[kk]->fixed=true;//we can not modify this mixture
				//if(vecGM[kk]->splitScore<0.0) vecGM[kk]->splitScore=0.0;//to avoid rounding errors
			}
		}


		cout<<"Number of proposed splits="<<splitSet.size()<<endl;
		//debug--------------------------------------------
		sprintf(buffer,"%.4d",frame);
		itoa=string(buffer);
		if (stat( (debugPath+"XML_splitCandidates").c_str(), &St ) != 0)//check if folder exists
		{
			cmd=string("mkdir " + debugPath + "XML_splitCandidates");
			error=system(cmd.c_str());
		}
		GMxmlFilename=string(debugPath+"XML_splitCandidates/"+"GMEMsplitCandidates_frame"+ itoa + ".xml");
		outXML.open(GMxmlFilename.c_str());
		GaussianMixtureModel::writeXMLheader(outXML);
		for(unsigned int ii=0;ii<vecGM.size();ii++)
		{
#ifdef WORK_WITH_UNITS
			vecGM[ii]->units2pixels(scaleOrig);
			vecGM[ii]->writeXML(outXML);
			vecGM[ii]->pixels2units(scaleOrig);
#else
			vecGM[ii]->writeXML(outXML);
#endif
		}
		GaussianMixtureModel::writeXMLfooter(outXML);
		outXML.close();
		//-------------------------------------------------------------------------
		if(splitSet.empty()==false)
		{
			
			//--------------------------------calculate local likelihood around all Gaussians that have divided (likelihood before division)---------------------------
			//vecGMHOST still contains the information before split
			//create a list of supervoxel indexes that are used for each local likelihood
			vector< vector<int> > listSupervoxelIdx( dividingNucleiIdx.size() );
			vector< list<supervoxel>::iterator > listSupervoxelIterators; 
			lht.getSupervoxelListIteratorsAtTM(listSupervoxelIterators, frame);
			int countSN = 0;
			for(list< supervoxel>::iterator iter = lht.supervoxelsList[frame].begin(); iter != lht.supervoxelsList[frame].end(); ++iter, ++countSN)
				iter->tempWildcard = (float) countSN;//so I can retrieve ordering

			countSN = 0;
			for(vector<int>::iterator iter = dividingNucleiIdx.begin(); iter != dividingNucleiIdx.end(); ++iter, ++countSN)
			{
				listSupervoxelIdx[countSN].reserve(configOptions.KmaxNumNNsupervoxel + 1);
				for(int ii = 0; ii<vecGMHOST[ *iter ].supervoxelNum; ii++)
				{
					int supervoxelIdxAux =  vecGMHOST[ *iter ].supervoxelIdx[ii];
					listSupervoxelIdx[countSN].push_back( supervoxelIdxAux );
					//we only consider the local likelihood in the supervoxels belonging to the dividing cell (very local). Otherwise test is not so meaningful
					//TODO: propose split using supervoxels (not K-means with local intensity). There is finite number of combinations
					//for(vector< SibilingTypeSupervoxel >::iterator iterS = listSupervoxelIterators[ supervoxelIdxAux ]->nearestNeighborsInSpace.begin(); iterS != listSupervoxelIterators[ supervoxelIdxAux ]->nearestNeighborsInSpace.end(); ++iterS)
					//	listSupervoxelIdx[countSN].push_back(( (int) ((*iterS)->tempWildcard) ) );
				}
			}
			//calculate and store likelihood for each of the elements
			vector<double> localLikelihoodBeforeSplit;
			localLikelihoodBeforeSplit.reserve( dividingNucleiIdx.size() );

			calculateLocalLikelihood(localLikelihoodBeforeSplit, listSupervoxelIdx, queryCUDA, imgDataCUDA, rnkCUDA, indCUDA, vecGMHOST, labelListPtrCUDA, query_nb, vecGM_ref_nb, numLabels);

			delete[] vecGMHOST;
			//-------------------------------------------------------------------------------------------------------------------------
			
			//string debugPathCUDAb(debugPath+"XML_GMEMiterations_CUDA_afterSplit_frame");
			string debugPathCUDAb("");
			vecGMHOST=new GaussianMixtureModelCUDA[vecGM.size()];
			for(unsigned int ii=0;ii<vecGM.size();ii++) copy2GMEM_CUDA(vecGM[ii],&(vecGMHOST[ii]));
			//GMEMvariationalInferenceCUDA(queryCUDA,imgDataCUDA,rnkCUDA,rnkCUDAtr,indCUDA,indCUDAtr,vecGMHOST,query_nb,vecGM.size(),configOptions.maxIterEM,configOptions.tolLikelihood,devCUDA,frame,true,debugPathCUDAb);
			GMEMvariationalInferenceCUDAWithSupervoxels(queryCUDA,imgDataCUDA,rnkCUDA,rnkCUDAtr,indCUDA,indCUDAtr,centroidLabelPositionCUDA,labelListPtrCUDA,vecGMHOST,query_nb,vecGM.size(),numLabels,configOptions.maxIterEM,configOptions.tolLikelihood,devCUDA,frame,regularize_W4DOF, debugPathCUDAb);
			for(unsigned int ii=0;ii<vecGM.size();ii++) copyFromGMEM_CUDA(&(vecGMHOST[ii]),vecGM[ii]);


			//--------------------------------calculate local likelihood around all Gaussians that have divided---------------------------
			vector<double> localLikelihoodAfterSplit;
			localLikelihoodAfterSplit.reserve( dividingNucleiIdx.size() );
			calculateLocalLikelihood(localLikelihoodAfterSplit, listSupervoxelIdx, queryCUDA, imgDataCUDA, rnkCUDA, indCUDA, vecGMHOST, labelListPtrCUDA, query_nb, vecGM.size(), numLabels);

			//calculate final likelihood test ratio
			vector<double> splitScoreVec( dividingNucleiIdx.size() );
			for(size_t aa = 0; aa < dividingNucleiIdx.size(); aa++)
				splitScoreVec[aa] = vecGM[ dividingNucleiIdx[aa] ]->splitScore;
			 likelihoodRatioTestSplitMerge(localLikelihoodBeforeSplit, localLikelihoodAfterSplit, splitScoreVec, splitMergeTest);
			//-------------------------------------------------------------------------------------------------------------------------
			
			delete[] vecGMHOST;


			//debug--------------------------------------------
			sprintf(buffer,"%.4d",frame);
			itoa=string(buffer);
			if (stat( (debugPath+"XML_finalSecondRoundEMSplit").c_str(), &St ) != 0)//check if folder exists
			{
				cmd=string("mkdir " + debugPath + "XML_finalSecondRoundEMSplit");
				error=system(cmd.c_str());
			}

			GMxmlFilename=string(debugPath+ "XML_finalSecondRoundEMSplit/GMEMfinalSecondRoundEMSplit_frame"+ itoa + ".xml");
			string xmlFilenameAfterSplit=GMxmlFilename;
			outXML.open(GMxmlFilename.c_str());
			GaussianMixtureModel::writeXMLheader(outXML);
			for(unsigned int ii=0;ii<vecGM.size();ii++)
			{
#ifdef WORK_WITH_UNITS
			vecGM[ii]->units2pixels(scaleOrig);
			vecGM[ii]->writeXML(outXML);
			vecGM[ii]->pixels2units(scaleOrig);
#else
			vecGM[ii]->writeXML(outXML);
#endif
			}
			GaussianMixtureModel::writeXMLfooter(outXML);
			outXML.close();
			//-------------------------------------------------------------------------
			//likelihood test ratio to decide which cell divisions where successful (done in the GPU now)

			numDiv=0;
			int pos=0;
			for(unsigned int kk=0;kk<splitSet.size();kk++)
			{
				if(splitSet[kk].first->isDead() || splitSet[kk].second->isDead())//if one cell is dead, then whether the other cell is alive or also dead the action is the same: delete second cell and restore previous GM
				{

					pos=splitSet[kk].second->id;
					for(unsigned int ss=pos+1;ss<vecGM.size();ss++) vecGM[ss]->id--;
					delete vecGM[pos];
					vecGM.erase(vecGM.begin()+pos);

					//reset Gaussian parameters to before the split
					pos=splitSet[kk].first->id;
					*(splitSet[kk].first)=backupVecGM[kk];//reset Gaussian parameters
					splitSet[kk].first->id=pos;
				}else{//both cell divisions are alive
					//if((splitSet[kk].second->splitScore+splitSet[kk].first->splitScore)>singleCellSplitScore[kk])//division proposal was not accepted
					//if(max(splitSet[kk].second->splitScore,splitSet[kk].first->splitScore)>singleCellSplitScore[kk])//division proposal was not accepted
					if(splitMergeTest[kk]<0)//division proposal rejected
					{
						pos=splitSet[kk].second->id;
						for(unsigned int ss=pos+1;ss<vecGM.size();ss++) vecGM[ss]->id--;
						delete vecGM[pos];
						vecGM.erase(vecGM.begin()+pos);

						pos=splitSet[kk].first->id;
						*(splitSet[kk].first)=backupVecGM[kk];//reset Gaussian parameters
						splitSet[kk].first->id=pos;
					}else{//division proposal was accepted by the likelihood ratio test
						numDiv++;
						//update priors since they were changed to test splits
						splitSet[kk].first->updatePriors(configOptions.betaPercentageOfN_k,configOptions.nuPercentageOfN_k,configOptions.alphaPercentage, -1.0);//alpha is not altered
						splitSet[kk].second->updatePriors(configOptions.betaPercentageOfN_k,configOptions.nuPercentageOfN_k, configOptions.alphaPercentage, -1.0);//alpha is not altered
					}
				}
			}

			cout<<"Accepted "<<numDiv<<" cell divisions"<<endl;

			//special case for first frame in order to create a single lineage per initial nuclei after division
			if(frame == iniFrame && argc<5)
			{
				//redo all the lineages id
				int llId = 0;
				for(vector<GaussianMixtureModel*>::iterator iter = vecGM.begin(); iter != vecGM.end(); ++iter)
				{
					(*iter)->lineageId = llId;
					llId++;
				}
			}


			//string debugPathCUDAc(debugPath+"XML_GMEMiterations_CUDA_finalRound_frame");
			sprintf(buffer,"%.4d",frame);//to save rnk and ind for each supervoxel
			itoa=string(buffer);
			string debugPathCUDAc(debugPath+"XML_finalResult");
			if (stat( debugPathCUDAc.c_str(), &St ) != 0)//check if folder exists
			{
				cmd=string("mkdir " + debugPathCUDAc);
				error=system(cmd.c_str());
			}
			debugPathCUDAc=string(debugPathCUDAc+ "/rnk_frame"+ itoa + ".bin");

			GaussianMixtureModelCUDA *vecGMHOST=new GaussianMixtureModelCUDA[vecGM.size()];
			for(unsigned int ii=0;ii<vecGM.size();ii++) copy2GMEM_CUDA(vecGM[ii],&(vecGMHOST[ii]));
			//GMEMvariationalInferenceCUDA(queryCUDA,imgDataCUDA,rnkCUDA,rnkCUDAtr,indCUDA,indCUDAtr,vecGMHOST,query_nb,vecGM.size(),configOptions.maxIterEM,configOptions.tolLikelihood,devCUDA,frame,true,debugPathCUDAc);
			GMEMvariationalInferenceCUDAWithSupervoxels(queryCUDA,imgDataCUDA,rnkCUDA,rnkCUDAtr,indCUDA,indCUDAtr,centroidLabelPositionCUDA,labelListPtrCUDA,vecGMHOST,query_nb,vecGM.size(),numLabels,configOptions.maxIterEM,configOptions.tolLikelihood,devCUDA,frame,regularize_W4DOF, debugPathCUDAc);
			for(unsigned int ii=0;ii<vecGM.size();ii++) copyFromGMEM_CUDA(&(vecGMHOST[ii]),vecGM[ii]);
			delete[] vecGMHOST;

		}//end of if(splitSet.empty==false)

		//allow all the mixtures to be modified again
		unfixedAllMixtures(vecGM);

		//-------------------write out final result for frame-------------------------
		sprintf(buffer,"%.4d",frame);
		itoa=string(buffer);
		if (stat( (debugPath+"XML_finalResult").c_str(), &St ) != 0)//check if folder exists
		{
			cmd=string("mkdir " + debugPath + "XML_finalResult");
			error=system(cmd.c_str());
		}

		GMxmlFilename=string(debugPath+"XML_finalResult/GMEMfinalResult_frame"+ itoa + ".xml");
		outXML.open(GMxmlFilename.c_str());
		GaussianMixtureModel::writeXMLheader(outXML);
		for(unsigned int ii=0;ii<vecGM.size();ii++)
		{
#ifdef WORK_WITH_UNITS
			vecGM[ii]->units2pixels(scaleOrig);
			vecGM[ii]->writeXML(outXML);
			vecGM[ii]->pixels2units(scaleOrig);
#else
			vecGM[ii]->writeXML(outXML);
#endif
		}
		GaussianMixtureModel::writeXMLfooter(outXML);
		outXML.close();
		//------------------------------------------------------------------

#endif //CELL_DIVISION_WITH_GPU_3DHAAR

		//---------------------------------------------------------------------------------------------------
		//-------------incorporate temporal logical rules in a time window to improve results----------------
		TicTocTimer ttTemporalLogicalRules = tic();
		//-----update lineage hyper tree with the final GMM results from this frame-----------------------
		list<nucleus> *listNucleiPtr = (&( lht.nucleiList[frame] ) );
		list<nucleus>::iterator listNucleiIter;
		nucleus nucleusAux;
		nucleusAux.TM = frame;


		vector< list<supervoxel>::iterator > vecSupervoxelsIter;
		lht.getSupervoxelListIteratorsAtTM(vecSupervoxelsIter, frame);

		vector< list<nucleus>::iterator > vecNucleiIter;
		if(frame > iniFrame)
			lht.getNucleiListIteratorsAtTM(vecNucleiIter, frame-1);//we need this to retrieve lineage

		for(size_t ii = 0; ii<vecGM.size(); ii++)
		{
			if(vecGM[ii]->isDead() == true) continue;
			if( vecGM[ii]->supervoxelIdx.empty() ) continue;//there was no clear assignment for this nuclei


			/*
			if( vecGM[ii]->supervoxelIdx.size() == 1 )//TOD0: sometimes, there is larger discrepancy between vecGM centorid and supervoxel centroid (even when there is only one super-voxel). this is a quick hack
			{
				for(int aa = 0;aa<dimsImage;aa++)
					nucleusAux.centroid[aa] = vecSupervoxelsIter[ vecGM[ii]->supervoxelIdx[0] ]->centroid[aa];
			}else{
				for(int aa = 0;aa<dimsImage;aa++)
					nucleusAux.centroid[aa] = vecGM[ii]->m_k(aa);
			}
			*/

			nucleusAux.avgIntensity = vecGM[ii]->N_k;			

			listNucleiPtr->push_back(nucleusAux);
			listNucleiIter = ((++ ( listNucleiPtr->rbegin() ) ).base());//iterator for the last element in the list
			
			//add supervoxel-nuclei relationship	
			for(size_t aa = 0; aa < vecGM[ii]->supervoxelIdx.size();aa++)
			{
				listNucleiIter->addSupervoxelToNucleus(vecSupervoxelsIter[ vecGM[ii]->supervoxelIdx[aa] ]);
				vecSupervoxelsIter[ vecGM[ii]->supervoxelIdx[aa] ]->treeNode.setParent(listNucleiIter);
			}

			 
			//add lineaging relationships			
			if(vecGM[ii]->parentId >= 0 ) //parentId contains the position of the parent nucleus
			{
				listNucleiIter->treeNode.setParent ( vecNucleiIter[ vecGM[ii]->parentId ] -> treeNode.getParent() );
				listNucleiIter->treeNode.getParent()->bt.SetCurrent( vecNucleiIter[ vecGM[ii]->parentId ] ->treeNodePtr );
				listNucleiIter->treeNodePtr = listNucleiIter->treeNode.getParent()->bt.insert(listNucleiIter);
				if(listNucleiIter->treeNodePtr == NULL)
					return 3;//error
				

			}else{//new lineage
				lht.lineagesList.push_back( lineage() );
				list<lineage>::iterator listLineageIter = ((++ ( lht.lineagesList.rbegin() ) ).base());//iterator for the last element in the list

				listNucleiIter->treeNode.setParent(listLineageIter);
				listNucleiIter->treeNodePtr = listLineageIter->bt.insert(listNucleiIter);
				if(listNucleiIter->treeNodePtr == NULL)
					return 3;//error
			}

			//calculate centroid			
			lht.calculateNucleiIntensityCentroid<float>(listNucleiIter);
		}			

		

		//-------------------------check if there are possible cell divisions(greedy)--------------------------------------------------	
		int numCellDivisions;
		int numBirths;
		cellDivisionMinFlowTouchingSupervoxels(lht, frame, minNeighboringVoxelsConn3D, minNeighboringVoxels, numCellDivisions, numBirths);
		cout<<"Generated (not touching supervoxels) "<<numCellDivisions<<" cell divisions out of "<<lht.nucleiList[frame].size() - numCellDivisions<<" cells in frame "<<frame<<".Also added "<<numBirths<<" new tracks"<<endl;
		


		//-------perform modifications using temporal logical rules----------------------------------
		//my sliding window is t \in [frame - 2 * configOptions.temporalWindowRadiusForLogicalRules, frame] => size of sliding window is 2 * configOptions.temporalWindowRadiusForLogicalRules + 1
		//lht still contains Tm frame - 2 * configOptions.temporalWindowRadiusForLogicalRules - 1 as an "anchor" time point: it cannot be modified by sliding window => solution should be consistent with it (we cannot merge two lineages present at athe acnhor time point)
			
		
		//parameters for logical temporal rules corrections (TODO: I should add them to some advance panel options later)		
		if(frame >= iniFrame + 2 * configOptions.temporalWindowRadiusForLogicalRules)
		{
			int numCorrections, numSplits;

#ifdef CELL_DIVISION_WITH_GPU_3DHAAR
			lht.mergeShortLivedAndCloseByDaughtersAll(lengthTMthr, frame - lengthTMthr, minNeighboringVoxels, minNeighboringVoxelsConn3D, numCorrections, numSplits);//merge first cell divisions
			cout<<"Merged "<<numCorrections<<" out of "<<numSplits<<" splits because of a sibling death before "<<lengthTMthr<<" time points after cell division and touching tracks"<<endl;
#else			
			//lht.mergeNonSeparatingDaughtersAll(frame - 2 * configOptions.temporalWindowRadiusForLogicalRules + 1, minNeighboringVoxels, minNeighboringVoxelsConn3D, numCorrections, numSplits);//merge first cell divisions
			//cout<<"Merged "<<numCorrections<<" out of "<<numSplits<<" splits because of touching tracks"<<endl;
			lht.deleteShortLivedDaughtersAll(lengthTMthr, frame - lengthTMthr,numCorrections, numSplits);//delete short living daughter
			cout<<"Deleted "<<numCorrections<<" out of "<<numSplits<<" splits because of a sibling death before "<<lengthTMthr<<" time points after cell division"<<endl;

#endif
			
			extendDeadNucleiAtTMwithHS(lht, hs,  frame-1,numCorrections, numSplits);//strictly speaking this is not a temporal feature, since it does not require a window, but it is still better to do it here (we can extend later)
			cout<<"Extended "<<numCorrections<<" out of "<<numSplits<<" dead cells using a simple local Hungarian algorithm with supervoxels"<<endl;


			//redo in case other fixes have incorporated 
			lht.deleteShortLivedDaughtersAll(lengthTMthr, frame - lengthTMthr,numCorrections, numSplits);//delete short living daughter
			cout<<"Deleted "<<numCorrections<<" out of "<<numSplits<<" splits because of a sibling death before "<<lengthTMthr<<" time points after cell division"<<endl;


			//background detection is always run (then we can decide what to do with the results)
			if( configOptions.temporalWindowRadiusForLogicalRules < temporalWindowSizeForBackground )
			{
				cout<<"ERROR: mainTrackingGaussianMixtureModel: temporalWindowRadiusForLogicalRules cannot be smaller than temporalWindowSizeForBackground"<<endl;
				exit(3);
			}

			time_t t_start, t_end;
			time(&t_start);
			//store the probability of being background for each nuclei
			err = setProbBackgroundTracksAtTM(lht, frame - configOptions.temporalWindowRadiusForLogicalRules , temporalWindowSizeForBackground, backgroundClassifierFilename, devCUDA);
			if( err > 0 )
				return err;
			time(&t_end);
			cout<<"Scored background tracks in "<<difftime(t_end, t_start)<<" secs"<<endl; 


			//--------------------------------------------------------
			//This is just a patch tomake sure that breakCellDivisionBasedOnCellDivisionPlaneConstraint works correctly. Som of the heuristic spatio-temporal rules change nuclei composition of supervoxels but do not update centroids.
			//Thus, in some cases teh midplane feature was miscalculated. This a quick fix to avoid that
			//TODO: find which heuristic rule changes the centroid and needs to be recalculated
			int TMaux = frame - 2 * configOptions.temporalWindowRadiusForLogicalRules;
			for(list<nucleus>::iterator iterN = lht.nucleiList[TMaux].begin(); iterN != lht.nucleiList[TMaux].end(); ++iterN)	
			{
				TreeNode<ChildrenTypeLineage>* aux = iterN->treeNodePtr;

				if( aux->getNumChildren() != 2 )
					continue;//not a cell division
				
				//recalculate centroid for mother and two daughters
				lht.calculateNucleiIntensityCentroid<float>(iterN);
				lht.calculateNucleiIntensityCentroid<float>(iterN->treeNodePtr->left->data);
				lht.calculateNucleiIntensityCentroid<float>(iterN->treeNodePtr->right->data);
			}			
			//-----------------------------------------------------------			
			

			//analyze cell divisions and cut links in the ones that do not satisfy the midplane division constraint
			err = lht.breakCellDivisionBasedOnCellDivisionPlaneConstraint(frame - 2 * configOptions.temporalWindowRadiusForLogicalRules, configOptions.thrCellDivisionPlaneDistance ,numCorrections, numSplits);//delete short living daughter
			if( err > 0 )
				return err;
			cout<<"Cut "<<numCorrections<<" linkages out of "<<numSplits<<" cell divisions because it did not satisfy the cell division midplane constraint"<<endl;		

		}else if (frame == iniFrame + 2 * configOptions.temporalWindowRadiusForLogicalRules - 1) 
		{	
			//special case to merge tracks due to initial oversegmentation			
			int numCorrections, numSplits, numMerges;
#ifdef CELL_DIVISION_WITH_GPU_3DHAAR
			lht.mergeShortLivedAndCloseByDaughtersAll(lengthTMthr, frame - lengthTMthr, minNeighboringVoxels, minNeighboringVoxelsConn3D, numCorrections, numSplits);//merge first cell divisions
			cout<<"Merged "<<numCorrections<<" out of "<<numSplits<<" splits because of a sibling death before "<<lengthTMthr<<" time points after cell division and touching tracks"<<endl;
#else
			//lht.mergeNonSeparatingDaughtersAll(frame - 2 * configOptions.temporalWindowRadiusForLogicalRules + 1, minNeighboringVoxels, minNeighboringVoxelsConn3D, numCorrections, numSplits);//merge first cell divisions
			//cout<<"Merged "<<numCorrections<<" out of "<<numSplits<<" splits because of touching tracks"<<endl;
			lht.deleteShortLivedDaughtersAll(lengthTMthr, frame - lengthTMthr,numCorrections, numSplits);////delete short living daughter
			cout<<"Deleted "<<numCorrections<<" out of "<<numSplits<<" splits because of a sibling death before "<<lengthTMthr<<" time points after cell division"<<endl;
#endif
			
			//delete all the tracks that have died during this "burn in" period
			lht.deleteDeadBranchesAll(frame, numMerges);
			cout<<"Deleted "<<numMerges<<" lineages that died during the burning period"<<endl;				


		}

		//-------------------------------------------------------------------------------------
		//---------------------re-calculate thrDist2LargeDisplacement based on current data-----
		if( frame > iniFrame )
		{
			vector<float> dispVec;
			dispVec.reserve( lht.nucleiList[frame].size() );

			float auxDist;
			for( list<nucleus>::iterator iterNN = lht.nucleiList[frame].begin(); iterNN != lht.nucleiList[frame].end(); ++iterNN )
			{
				if( iterNN->treeNodePtr->parent != NULL )
				{
					auxDist = iterNN->treeNodePtr->data->Euclidean2Distance( *(iterNN->treeNodePtr->parent->data), supervoxel::getScale());
					dispVec.push_back( sqrt(auxDist) );


					//if displacement is way too large->flag it as background (it might not be, but we want to clean these results anyways)
					if( auxDist > 6.25 * thrDist2LargeDisplacement )//because we use squares, if we want k times above thrDist2LargeDisplacement->k*k  (for us, k = 2.5 -> 10 std are just not acceptable)
						iterNN->probBackground = 1.0f;
				}
			}
			if( dispVec.size() > 30 )//for reliable statistics. Otherwise we do not update
			{
				//calculate median
				std::sort(dispVec.begin(), dispVec.end());
				float auxMedian = dispVec[ (size_t)(dispVec.size() / 2) ];
				//calculate median absolute deviation
				for(size_t ii =0; ii < dispVec.size(); ii++)
				{
					dispVec[ii]-= auxMedian;
					dispVec[ii] = fabs( dispVec[ii] );
				}
				std::sort(dispVec.begin(), dispVec.end());
				float auxMAD = dispVec[ (size_t)(dispVec.size() / 2) ];
				thrDist2LargeDisplacement = auxMedian + 4.0 *  auxMAD * 1.4826 ;//sigma = 1.4826 * MAD -> 4 std are considered outliers
				thrDist2LargeDisplacement = thrDist2LargeDisplacement * thrDist2LargeDisplacement;
			}
		}
		cout<<"Updated thrDist2LargeDisplacement to = "<<sqrt(thrDist2LargeDisplacement)<<" * "<<sqrt(thrDist2LargeDisplacement)<<endl;
		//-------------------------------------------------------------------------------------


		//------save last element before being removed of the window----------------------------------------
		if(frame >= iniFrame + 2 * configOptions.temporalWindowRadiusForLogicalRules )
		{
			int frameOffset = frame - 2 * configOptions.temporalWindowRadiusForLogicalRules;
			//save time point frame - 2 * configOptions.temporalWindowRadiusForLogicalRules using GMM format
			parseNucleiList2TGMM<float>(vecGM,lht,frameOffset, true, thrDist2LargeDisplacement);//the only thing I need to modify is the parentId and the the neigh			


			//set wildcard to idx for frameOffset-1 so we can set parentIdx
			if( frameOffset > 0)
			{
				int countV = 0;
				for(list< nucleus >::iterator iterN = lht.nucleiList[frameOffset-1].begin(); iterN != lht.nucleiList[frameOffset-1].end(); ++iterN,++countV)
					iterN->tempWilcard = (float)countV;
			}
			sprintf(buffer,"%.4d",frameOffset);
			itoa=string(buffer);
			if (stat( (debugPath+"XML_finalResult_lht").c_str(), &St ) != 0)//check if folder exists
			{
				cmd=string("mkdir " + debugPath + "XML_finalResult_lht");
				error=system(cmd.c_str());
			}
			GMxmlFilename = string(debugPath+"XML_finalResult_lht/GMEMfinalResult_frame"+ itoa + ".xml");

			outXML.open(GMxmlFilename.c_str());
			GaussianMixtureModel::writeXMLheader(outXML);
			int countV = 0;
			for(list< nucleus >::iterator iterN = lht.nucleiList[frameOffset].begin(); iterN != lht.nucleiList[frameOffset].end(); ++iterN, ++countV)
			{
				//modify parentId
				if(iterN->treeNodePtr->parent == NULL)
					vecGM[countV]->parentId = -1;
				else{
					vecGM[countV]->parentId = (int)(iterN->treeNodePtr->parent->data->tempWilcard);
				}
				//write solution
				vecGM[countV]->writeXML(outXML);
			}
			GaussianMixtureModel::writeXMLfooter(outXML);
			outXML.close();

			//save supervoxels so I can visualize them 
			string filenameSv(debugPath+"XML_finalResult_lht/GMEMfinalResult_frame"+ itoa + ".svb");
			if( lht.writeListSupervoxelsToBinaryFile( filenameSv, frameOffset ) > 0 )
				exit(3);

			//delete time point frame - 2 * configOptions.temporalWindowRadiusForLogicalRules - 1 since it is not needed as an anchor point anymore
			lht.setFrameAsT_o(frameOffset);

			//delete hierarchical segmentation for this time point
			delete hsVec.front();
			hsVec.erase(hsVec.begin());
		}
		cout<<"Applying all the temporal logical rules took "<<toc(&ttTemporalLogicalRules)<<" secs"<<endl;
		//--------------end of temporal logical rules--------------------------------------------------
		//---------------------------------------------------------------------------------------------


		

		//release memory for each frame
		//mylib::Free_Array(img);//this memory will be freed by supervoxels (we need the image for the temporal sliding window)
		img=NULL;
		
		if(imgUINT16!=NULL)
		{
			mylib::Free_Array(imgUINT16);
			imgUINT16 = NULL;
		}
		if(imgFlow!=NULL)
		{
			mylib::Free_Array(imgFlow);
			imgFlow=NULL;
		}
		if(imgFlowMask!=NULL)
		{
			mylib::Free_Array(imgFlowMask);
			imgFlowMask=NULL;
			imgFlowMaskPtr=NULL;
		}


		GMEMreleaseMemoryWithSupervoxels(&queryCUDA,&imgDataCUDA,&rnkCUDA,&indCUDA,&indCUDAtr,&centroidLabelPositionCUDA,&labelListPtrCUDA);
		delete []queryHOST;
		delete []imgDataHOST;
		delete []centroidLabelPositionHOST;
		delete []centroidLabelWeightHOST;
		delete []labelListPtrHOST;

		cout<<toc(&tt)<<" secs"<<endl;


		//------------------------------------------------------------------------------
		//check number of deaths and if we need to rerun the same point with optical flow
		if(configOptions.deathThrOpticalFlow>=0 && frame!=iniFrame) //for iniFrame we can not compute flow
		{
			if(configOptions.estimateOpticalFlow==2)//it means we already tried to apply optical flow in this frame
			{
				configOptions.estimateOpticalFlow=0;//we just set to zeroto avoid infinite loop in case optical flow does not fix death issue
			}else{
				numDeaths=0;
				list<int> parentDeathList;
				for(unsigned int kk=0;kk<vecGM.size();kk++)
				{	if(vecGM[kk]->isDead()==true)
					{
						numDeaths++;
						parentDeathList.push_back(vecGM[kk]->parentId);
					}
				}
				//activate module to calculate optical flow and redo this last frame
				if(numDeaths>=configOptions.deathThrOpticalFlow)
				{
					cout<<"Number of dead cells in this frame "<<numDeaths<<" is above threshold. We need to rerun with optical flow"<<endl;
					frame--;

					//delete existing solution
					for(unsigned int ii=0;ii<vecGM.size();ii++) delete vecGM[ii];
					vecGM.clear();

					//reload previous solution
					char extra[128];
					sprintf(extra,"%.4d",frame);
					string itoaD=string(extra);
					string GMxmlFilename(debugPath+"XML_finalResult/GMEMfinalResult_frame"+ itoaD + ".xml");
					XMLNode xMainNode=XMLNode::openFileHelper(GMxmlFilename.c_str(),"document");
					int n=xMainNode.nChildNode("GaussianMixtureModel");
					for(int ii=0;ii<n;ii++) vecGM.push_back(new GaussianMixtureModel(xMainNode,ii));

					memcpy(scaleOrig,GaussianMixtureModel::scale,sizeof(float)*dimsImage);//to save in case we work with units
#ifdef WORK_WITH_UNITS
					//transform all given positions
					for(unsigned int ii=0;ii<vecGM.size();ii++) vecGM[ii]->pixels2units(scaleOrig);
#endif

					for(unsigned int kk=0;kk<vecGM.size();kk++)
					{
						vecGM[kk]->updateNk();//update Nk just in case
					}

					//setup imgFlowMask to select which areas we need flow (around death cells)
					if(imgFlowMask==NULL)//reallocate memory
					{
						imgFlowMask=mylib::Make_Array(mylib::PLAIN_KIND,mylib::UINT8_TYPE,imgDims,imgSize);
						imgFlowMaskPtr=(mylib::uint8*)(imgFlowMask->data);
					}
					//reset mask
					memset(imgFlowMask->data,0,sizeof(mylib::uint8)*imgFlowMask->size);


					//---------------set imgFlowMask to 1 everywhere so we apply flow everywhere---------------------
					for(long long int aa=0;aa<imgFlowMask->size;aa++) imgFlowMaskPtr[aa]=1;										

					//activate flag so we compute optical flow in the next iteration of the for loop
					configOptions.estimateOpticalFlow=2;

				}else{
					//deactivate flag to compute optical flow
					configOptions.estimateOpticalFlow=0;
				}
			}
			cout<<"ERROR: with the update to lineage hyper tree the code is not ready yet to use vector flow calculations, sicne we need to update lht instead of vecGM"<<endl;
			exit(3);
		}
		//-----------------------------end of if(configOptions.deathThrOpticalFlow>0)--------------------------------
		

	}//end of for(frame=...) loop


	//--------------------------------------------------------------------------------
	//-----------flush out the last time points in the lineage that were not saved because of teh sliding window approach-------------
	for( int frame = endFrame + 1 - 2 * configOptions.temporalWindowRadiusForLogicalRules; frame <= endFrame; frame++)
	{	

		//analyze cell divisions and cut links in the ones that do not satisfy the midplane division constraint
		if( frame < endFrame )
		{
			int numCorrections, numSplits;
			int err = lht.breakCellDivisionBasedOnCellDivisionPlaneConstraint(frame, configOptions.thrCellDivisionPlaneDistance ,numCorrections, numSplits);//delete short living daughter
			if( err > 0 )
				return err;
			cout<<"Cut "<<numCorrections<<" linkages out of "<<numSplits<<" cell divisions because it did not satisfy the cell division midplane constraint (frame"<<frame<<")"<<endl;	
		}
		//save time point frame 
		parseNucleiList2TGMM<float>(vecGM,lht,frame, true, thrDist2LargeDisplacement);//the only thing I need to modify is the parentId and the the neigh

		//set wildcard to idx for frameOffset-1 so we can set parentIdx
		if( frame > 0)
		{
			int countV = 0;
			for(list< nucleus >::iterator iterN = lht.nucleiList[frame-1].begin(); iterN != lht.nucleiList[frame-1].end(); ++iterN,++countV)
				iterN->tempWilcard = (float)countV;
		}
		sprintf(buffer,"%.4d",frame);
		itoa=string(buffer);
		if (stat( (debugPath+"XML_finalResult_lht").c_str(), &St ) != 0)//check if folder exists
		{
			cmd=string("mkdir " + debugPath + "XML_finalResult_lht");
			error=system(cmd.c_str());
		}
		string GMxmlFilename = string(debugPath+"XML_finalResult_lht/GMEMfinalResult_frame"+ itoa + ".xml");

		ofstream outXML(GMxmlFilename.c_str());
		GaussianMixtureModel::writeXMLheader(outXML);
		int countV = 0;
		for(list< nucleus >::iterator iterN = lht.nucleiList[frame].begin(); iterN != lht.nucleiList[frame].end(); ++iterN, ++countV)
		{
			//modify parentId
			if(iterN->treeNodePtr->parent == NULL)
				vecGM[countV]->parentId = -1;
			else{
				vecGM[countV]->parentId = (int)(iterN->treeNodePtr->parent->data->tempWilcard);
			}
			//write solution
			vecGM[countV]->writeXML(outXML);
		}
		GaussianMixtureModel::writeXMLfooter(outXML);
		outXML.close();

		//save supervoxels so I can visualize them 
		string filenameSv(debugPath+"XML_finalResult_lht/GMEMfinalResult_frame"+ itoa + ".svb");
		if( lht.writeListSupervoxelsToBinaryFile( filenameSv, frame ) > 0 )
			exit(3);

		//delete hierarchical segmentation for this time point
		delete hsVec.front();
		hsVec.erase(hsVec.begin());
	}
	//-----------------------------------------------------



	//------------------------------------------------------
	//release memory
	for(unsigned int ii=0;ii<vecGM.size();ii++) delete vecGM[ii];
	vecGM.clear();
	outLog.close();
	Fx.clear();

	supervoxel::freeTrimParameters();


	

	//run background "cleaner"
	if( configOptions.thrBackgroundDetectorHigh < 1.0f )
	{
		TicTocTimer tt = tic();
		cout<<"Running forward-backward pass with hysteresis threshold background =("<<configOptions.thrBackgroundDetectorLow<<","<<configOptions.thrBackgroundDetectorHigh<<")"<<" to remove non-cell like objects"<<endl;
		
		string outputFolder( debugPath + "XML_finalResult_lht_bckgRm" );
		if (stat( outputFolder.c_str(), &St ) != 0)//check if folder exists
		{
			cmd=string("mkdir " + outputFolder);
			error=system(cmd.c_str());
		}
		
		int err = applyProbBackgroundHysteresisRulePerBranch(string(debugPath+"XML_finalResult_lht/GMEMfinalResult_frame"), iniFrame, endFrame, string(outputFolder + "/"), configOptions.thrBackgroundDetectorLow, configOptions.thrBackgroundDetectorHigh);
		cout<<toc(&tt)<<" secs"<<endl;
		if( err > 0 )
			return err;
	}


	return 0;
}



