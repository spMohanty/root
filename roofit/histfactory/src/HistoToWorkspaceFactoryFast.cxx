// @(#)root/roostats:$Id$
// Author: Kyle Cranmer, Akira Shibata
/*************************************************************************
 * Copyright (C) 1995-2008, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

//_________________________________________________
/*
BEGIN_HTML
<p>
</p>
END_HTML
*/
//


#ifndef __CINT__
#include "RooGlobalFunc.h"
#endif

#include "RooDataSet.h"
#include "RooRealVar.h"
#include "RooConstVar.h"
#include "RooAddition.h"
#include "RooProduct.h"
#include "RooProdPdf.h"
#include "RooAddPdf.h"
#include "RooGaussian.h"
#include "RooPoisson.h"
#include "RooExponential.h"
#include "RooRandom.h"
#include "RooCategory.h"
#include "RooSimultaneous.h"
#include "RooMultiVarGaussian.h"
#include "RooNumIntConfig.h"
#include "RooMinuit.h"
#include "RooNLLVar.h"
#include "RooProfileLL.h"
#include "RooFitResult.h"
#include "RooDataHist.h"
#include "RooHistFunc.h"
#include "RooHistPdf.h"
#include "RooRealSumPdf.h"
#include "RooProduct.h"
#include "RooWorkspace.h"
#include "RooCustomizer.h"
#include "RooPlot.h"
#include "RooMsgService.h"
#include "RooStats/RooStatsUtils.h"
#include "RooStats/ModelConfig.h"
#include "RooStats/HistFactory/PiecewiseInterpolation.h"
#include "RooStats/HistFactory/ParamHistFunc.h"
#include "RooStats/AsymptoticCalculator.h"

#include "TH2F.h"
#include "TH3F.h"
#include "TFile.h"
#include "TCanvas.h"
#include "TH1.h"
#include "TLine.h"
#include "TTree.h"
#include "TMarker.h"
#include "TStopwatch.h"
#include "TROOT.h"
#include "TStyle.h"
#include "TVectorD.h"
#include "TMatrixDSym.h"

// specific to this package
#include "RooStats/HistFactory/LinInterpVar.h"
#include "RooStats/HistFactory/FlexibleInterpVar.h"
#include "RooStats/HistFactory/HistoToWorkspaceFactoryFast.h"
#include "RooStats/HistFactory/Measurement.h"
#include "Helper.h"

#include <algorithm>

#define VERBOSE

#define alpha_Low "-5"
#define alpha_High "5"
#define NoHistConst_Low "0"
#define NoHistConst_High "2000"

// use this order for safety on library loading
using namespace RooFit ;
using namespace RooStats ;
using namespace std ;

ClassImp(RooStats::HistFactory::HistoToWorkspaceFactoryFast)

namespace RooStats{
namespace HistFactory{

  HistoToWorkspaceFactoryFast::HistoToWorkspaceFactoryFast() : 
       fNomLumi(1.0), fLumiError(0),   
       fLowBin(0), fHighBin(0)  
  {}

  HistoToWorkspaceFactoryFast::~HistoToWorkspaceFactoryFast(){
  }

  HistoToWorkspaceFactoryFast::HistoToWorkspaceFactoryFast(RooStats::HistFactory::Measurement& measurement ) :
    fSystToFix( measurement.GetConstantParams() ),
    fParamValues( measurement.GetParamValues() ),
    fNomLumi( measurement.GetLumi() ),
    fLumiError( measurement.GetLumi()*measurement.GetLumiRelErr() ),
    fLowBin( measurement.GetBinLow() ),
    fHighBin( measurement.GetBinHigh() ) {

    // Set Preprocess functions
    SetFunctionsToPreprocess( measurement.GetPreprocessFunctions() );

  }

  void HistoToWorkspaceFactoryFast::ConfigureWorkspaceForMeasurement( const std::string& ModelName, RooWorkspace* ws_single, Measurement& measurement ) {

    // Configure a workspace by doing any
    // necessary post-processing and by
    // creating a ModelConfig

    // Make a ModelConfig and configure it
    ModelConfig * proto_config = (ModelConfig *) ws_single->obj("ModelConfig");
    if( proto_config == NULL ) {
      std::cout << "Error: Did not find 'ModelConfig' object in file: " << ws_single->GetName() 
		<< std::endl;
      throw hf_exc();
    }

    std::vector<std::string> poi_list = measurement.GetPOIList();
    if( poi_list.size()==0 ) {
      std::cout << "Warining: No Parametetrs of interest are set" << std::endl;
    }

    cout << "Setting Parameter(s) of Interest as: ";
    for(unsigned int i = 0; i < poi_list.size(); ++i) {
      cout << poi_list.at(i) << " ";
    }
    cout << endl;

    RooArgSet * params= new RooArgSet;
    for( unsigned int i = 0; i < poi_list.size(); ++i ) {
      std::string poi_name = poi_list.at(i);
      RooRealVar* poi = (RooRealVar*) ws_single->var( poi_name.c_str() );
      if(poi){
	params->add(*poi);
      }
      else {
	std::cout << "WARNING: Can't find parameter of interest: " << poi_name 
		  << " in Workspace. Not setting in ModelConfig." << std::endl;
	//throw hf_exc();
      }
    }
    proto_config->SetParametersOfInterest(*params);

    // Name of an 'edited' model, if necessary
    std::string NewModelName = "newSimPdf"; // <- This name is hard-coded in HistoToWorkspaceFactoryFast::EditSyt.  Probably should be changed to : std::string("new") + ModelName;

    // Activate Additional Constraint Terms
    if(    measurement.GetGammaSyst().size() > 0 
	|| measurement.GetUniformSyst().size() > 0 
	|| measurement.GetLogNormSyst().size() > 0 
	|| measurement.GetNoSyst().size() > 0) {
      HistoToWorkspaceFactoryFast::EditSyst( ws_single, (ModelName).c_str(), 
					     measurement.GetGammaSyst(), 
					     measurement.GetUniformSyst(), 
					     measurement.GetLogNormSyst(), 
					     measurement.GetNoSyst());

      proto_config->SetPdf( *ws_single->pdf( "newSimPdf" ) );
    }
  
    // Set the ModelConfig's Params of Interest
    RooAbsData* expData = ws_single->data("asimovData");
    if( !expData ) {
      std::cout << "Error: Failed to find dataset: " << expData
		<< " in workspace" << std::endl;
      throw hf_exc();
    }
    if(poi_list.size()!=0){
      proto_config->GuessObsAndNuisance(*expData);
    }

    // Now, let's loop over any additional asimov datasets
    // that we need to make

    // Get the pdf
    // Notice that we get the "new" pdf, this is the one that is
    // used in the creation of these asimov datasets since they
    // are fitted (or may be, at least).
    RooAbsPdf* pdf = ws_single->pdf(NewModelName.c_str());
    if( !pdf ) pdf = ws_single->pdf( ModelName.c_str() );
    const RooArgSet* observables = ws_single->set("observables");

    // Create a SnapShot of the nominal values 
    std::string SnapShotName = "NominalParamValues";
    ws_single->saveSnapshot(SnapShotName.c_str(), ws_single->allVars());

    for( unsigned int i=0; i<measurement.GetAsimovDatasets().size(); ++i) {

      // Set the variable values and "const" ness with the workspace
      RooStats::HistFactory::Asimov& asimov = measurement.GetAsimovDatasets().at(i);
      std::string AsimovName = asimov.GetName();

      std::cout << "Generating additional Asimov Dataset: " << AsimovName << std::endl;
      asimov.ConfigureWorkspace(ws_single);
      RooDataSet* asimov_dataset = 
	(RooDataSet*) AsymptoticCalculator::GenerateAsimovData(*pdf, *observables);

      std::cout << "Importing Asimov dataset" << std::endl;
      double failure = ws_single->import(*asimov_dataset, Rename(AsimovName.c_str()));
      if( failure ) {
	std::cout << "Error: Failed to import Asimov dataset: " << AsimovName
		  << std::endl;
	throw hf_exc();
      }

      // Load the snapshot at the end of every loop iteration
      // so we start each loop with a "clean" snapshot
      ws_single->loadSnapshot(SnapShotName.c_str());

    }

    // Cool, we're done
    return; // ws_single;
  }


  // We want to eliminate this interface and use the measurment directly
  RooWorkspace* HistoToWorkspaceFactoryFast::MakeSingleChannelModel( Measurement& measurement, Channel& channel ) {

    // This is a pretty light-weight wrapper function
    //
    // Take a fully configured measurement as well as
    // one of its channels
    //
    // Return a workspace representing that channel
    // Do this by first creating a vector of EstimateSummary's
    // and this by configuring the workspace with any post-processing

    // Get the channel's name
    string ch_name = channel.GetName();

    // Create a workspace for a SingleChannel from the Measurement Object
    RooWorkspace* ws_single = this->MakeSingleChannelWorkspace(measurement, channel);
    if( ws_single == NULL ) {
      std::cout << "Error: Failed to make Single-Channel workspace for channel: " << ch_name
		<< " and measurement: " << measurement.GetName() << std::endl;
      throw hf_exc();
    }

    // Finally, configure that workspace based on
    // properties of the measurement
    HistoToWorkspaceFactoryFast::ConfigureWorkspaceForMeasurement( "model_"+ch_name, ws_single, measurement );

    return ws_single;

  }

  RooWorkspace* HistoToWorkspaceFactoryFast::MakeCombinedModel( Measurement& measurement ) {

    // This function takes a fully configured measurement
    // which may contain several channels and returns
    // a workspace holding the combined model
    //
    // This can be used, for example, within a script to produce 
    // a combined workspace on-the-fly
    //
    // This is a static function (for now) to make
    // it a one-liner

    // First, we create an instance of a HistFactory 
    HistoToWorkspaceFactoryFast factory( measurement );

    // Loop over the channels and create the individual workspaces
    vector<RooWorkspace*> channel_workspaces;
    vector<string>        channel_names;
    
    for( unsigned int chanItr = 0; chanItr < measurement.GetChannels().size(); ++chanItr ) {
    
      HistFactory::Channel& channel = measurement.GetChannels().at( chanItr );

      if( ! channel.CheckHistograms() ) {
	std::cout << "MakeModelAndMeasurementsFast: Channel: " << channel.GetName()
		  << " has uninitialized histogram pointers" << std::endl;
	throw hf_exc();
      }

      string ch_name = channel.GetName();
      channel_names.push_back(ch_name);

      // GHL: Renaming to 'MakeSingleChannelWorkspace'
      RooWorkspace* ws_single = factory.MakeSingleChannelModel( measurement, channel );
      
      channel_workspaces.push_back(ws_single);

    }

    
    // Now, combine the individual channel workspaces to
    // form the combined workspace
    RooWorkspace* ws = factory.MakeCombinedModel( channel_names, channel_workspaces );


    // Configure the workspace
    HistoToWorkspaceFactoryFast::ConfigureWorkspaceForMeasurement( "simPdf", ws, measurement );

    // Done.  Return the pointer
    return ws;

  }

  void HistoToWorkspaceFactoryFast::ProcessExpectedHisto(TH1* hist,RooWorkspace* proto, 
							 string prefix, string productPrefix, 
							 string systTerm ) {
    if(hist) {
      cout << "processing hist " << hist->GetName() << endl;
    } else {
      cout << "hist is empty" << endl;
      R__ASSERT(hist != 0); 
      return;                  
    }

    /// require dimension >=1 or <=3
    if (fObsNameVec.empty() && !fObsName.empty()) { fObsNameVec.push_back(fObsName); }    
    R__ASSERT( fObsNameVec.size()>=1 && fObsNameVec.size()<=3 );

    /// determine histogram dimensionality 
    unsigned int histndim(1);
    std::string classname = hist->ClassName();
    if      (classname.find("TH1")==0) { histndim=1; }
    else if (classname.find("TH2")==0) { histndim=2; }
    else if (classname.find("TH3")==0) { histndim=3; }
    R__ASSERT( histndim==fObsNameVec.size() );

    /// create roorealvar observables
    RooArgList observables;
    std::vector<std::string>::iterator itr = fObsNameVec.begin();
    for (int idx=0; itr!=fObsNameVec.end(); ++itr, ++idx ) {
      if ( !proto->var(itr->c_str()) ) {
	TAxis* axis(0);
	if (idx==0) { axis = hist->GetXaxis(); }
	if (idx==1) { axis = hist->GetYaxis(); }
	if (idx==2) { axis = hist->GetZaxis(); }
	Int_t nbins = axis->GetNbins();	
	Double_t xmin = axis->GetXmin();
	Double_t xmax = axis->GetXmax(); 	
	// create observable
	proto->factory(Form("%s[%f,%f]",itr->c_str(),xmin,xmax));
	proto->var(itr->c_str())->setBins(nbins);
      }
      observables.add( *proto->var(itr->c_str()) );
    }

    RooDataHist* histDHist = new RooDataHist((prefix+"nominalDHist").c_str(),"",observables,hist);
    RooHistFunc* histFunc = new RooHistFunc((prefix+"_nominal").c_str(),"",observables,*histDHist,0) ;

    proto->import(*histFunc);

    /// now create the product of the overall efficiency times the sigma(params) for this estimate
    proto->factory(("prod:"+productPrefix+"("+prefix+"_nominal,"+systTerm+")").c_str() );    

  }

  void HistoToWorkspaceFactoryFast::AddMultiVarGaussConstraint(RooWorkspace* proto, string prefix,int lowBin, int highBin, vector<string>& constraintTermNames){
    // these are the nominal predictions: eg. the mean of some space of variations
    // later fill these in a loop over histogram bins

    TVectorD mean(highBin); //-lowBin); // MB: fix range
    cout << "a" << endl;
    for(Int_t i=lowBin; i<highBin; ++i){
      std::stringstream str;
      str<<"_"<<i;
      RooRealVar* temp = proto->var((prefix+str.str()).c_str());
      mean(i) = temp->getVal();
    }

    TMatrixDSym Cov(highBin-lowBin);
    for(int i=lowBin; i<highBin; ++i){
      for(int j=0; j<highBin-lowBin; ++j){
        if(i==j) { Cov(i,j) = sqrt(mean(i)); } // MB : this doesn't make sense to me if lowBin!=0 (?)
	else { Cov(i,j) = 0; } 
      }
    }

    // can't make MultiVarGaussian with factory yet, do it by hand
    RooArgList floating( *(proto->set(prefix.c_str() ) ) );
    RooMultiVarGaussian constraint((prefix+"Constraint").c_str(),"",
             floating, mean, Cov);
             
    proto->import(constraint);

    constraintTermNames.push_back(constraint.GetName());
  }

  void HistoToWorkspaceFactoryFast::LinInterpWithConstraint(RooWorkspace* proto, TH1* nominal, 
							    std::vector<HistoSys> histoSysList,
							    string prefix, string productPrefix, 
							    string systTerm, 
							    vector<string>& constraintTermNames){

    // these are the nominal predictions: eg. the mean of some space of variations
    // later fill these in a loop over histogram bins

    // require dimension >=1 or <=3
    if (fObsNameVec.empty() && !fObsName.empty()) { fObsNameVec.push_back(fObsName); }    
    R__ASSERT( fObsNameVec.size()>=1 && fObsNameVec.size()<=3 );

    // determine histogram dimensionality 
    unsigned int histndim(1);
    std::string classname = nominal->ClassName();
    if      (classname.find("TH1")==0) { histndim=1; }
    else if (classname.find("TH2")==0) { histndim=2; }
    else if (classname.find("TH3")==0) { histndim=3; }
    R__ASSERT( histndim==fObsNameVec.size() );
    //    cout <<"In LinInterpWithConstriants and histndim = " << histndim <<endl;

    // create roorealvar observables
    RooArgList observables;
    std::vector<std::string>::iterator itr = fObsNameVec.begin();
    for (int idx=0; itr!=fObsNameVec.end(); ++itr, ++idx ) {
      if ( !proto->var(itr->c_str()) ) {
	TAxis* axis(NULL);
	if (idx==0) { axis = nominal->GetXaxis(); }
	else if (idx==1) { axis = nominal->GetYaxis(); }
	else if (idx==2) { axis = nominal->GetZaxis(); }
	else {
	  std::cout << "Error: Too many observables.  "
		    << "HistFactory only accepts up to 3 observables (3d) "
		    << std::endl;
	  throw hf_exc();
	}
	Int_t nbins = axis->GetNbins();	
	Double_t xmin = axis->GetXmin();
	Double_t xmax = axis->GetXmax(); 	
	// create observable
	proto->factory(Form("%s[%f,%f]",itr->c_str(),xmin,xmax));
	proto->var(itr->c_str())->setBins(nbins);
      }
      observables.add( *proto->var(itr->c_str()) );
    }

    RooDataHist* nominalDHist = new RooDataHist((prefix+"nominalDHist").c_str(),"",observables,nominal);
    RooHistFunc* nominalFunc = new RooHistFunc((prefix+"nominal").c_str(),"",observables,*nominalDHist,0) ;

    // make list of abstract parameters that interpolate in space of variations
    RooArgList params( ("alpha_Hist") );
    // range is set using defined macro (see top of the page)
    string range=string("[")+alpha_Low+","+alpha_High+"]";

    // Loop over the HistoSys list 
    for(unsigned int j=0; j<histoSysList.size(); ++j){
      std::stringstream str;
      str<<"_"<<j;

      HistoSys& histoSys = histoSysList.at(j);
      string histoSysName = histoSys.GetName();

      RooRealVar* temp = (RooRealVar*) proto->var(("alpha_" + histoSysName).c_str());
      if(!temp){

	temp = (RooRealVar*) proto->factory(("alpha_" + histoSysName + range).c_str());

        // now add a constraint term for these parameters
        string command=("Gaussian::alpha_"+histoSysName+"Constraint(alpha_"+histoSysName+",nom_alpha_"+histoSysName+"[0.,-10,10],1.)");
        cout << command << endl;
        constraintTermNames.push_back(  proto->factory( command.c_str() )->GetName() );
	proto->var(("nom_alpha_"+histoSysName).c_str())->setConstant();
	const_cast<RooArgSet*>(proto->set("globalObservables"))->add(*proto->var(("nom_alpha_"+histoSysName).c_str()));
      } 
      params.add(* temp );
    }

    // now make function that linearly interpolates expectation between variations
    // get low/high variations to interpolate between
    vector<double> low, high;
    RooArgSet lowSet, highSet;
    //ES// for(unsigned int j=0; j<lowHist.size(); ++j){
    for(unsigned int j=0; j<histoSysList.size(); ++j){
      std::stringstream str;
      str<<"_"<<j;

      HistoSys& histoSys = histoSysList.at(j);
      RooDataHist* lowDHist = new RooDataHist((prefix+str.str()+"lowDHist").c_str(),"",observables, histoSys.GetHistoLow());
      RooDataHist* highDHist = new RooDataHist((prefix+str.str()+"highDHist").c_str(),"",observables, histoSys.GetHistoHigh());
      RooHistFunc* lowFunc = new RooHistFunc((prefix+str.str()+"low").c_str(),"",observables,*lowDHist,0) ;
      RooHistFunc* highFunc = new RooHistFunc((prefix+str.str()+"high").c_str(),"",observables,*highDHist,0) ;
      lowSet.add(*lowFunc);
      highSet.add(*highFunc);
    }
    
    // this is sigma(params), a piece-wise linear interpolation
    PiecewiseInterpolation interp(prefix.c_str(),"",*nominalFunc,lowSet,highSet,params);
    interp.setPositiveDefinite();
    interp.setAllInterpCodes(0); // MB : change default to 1? = piece-wise log interpolation from pice-wise linear (=0)
    // KC: interpo codes 1 etc. don't have proper analytic integral.
    RooArgSet observableSet(observables);
    interp.setBinIntegrator(observableSet);
    interp.forceNumInt();

    proto->import(interp); // individual params have already been imported in first loop of this function
    
    // now create the product of the overall efficiency times the sigma(params) for this estimate
    proto->factory(("prod:"+productPrefix+"("+prefix+","+systTerm+")").c_str() );    

  }

  // GHL: Consider passing the NormFactor list instead of the entire sample
  string HistoToWorkspaceFactoryFast::AddNormFactor(RooWorkspace* proto, string& channel, string& sigmaEpsilon, Sample& sample, bool doRatio){
    string overallNorm_times_sigmaEpsilon ;
    string prodNames;

    vector<NormFactor> normList = sample.GetNormFactorList();
    vector<string> normFactorNames, rangeNames;

    if(normList.size() > 0){

      for(vector<NormFactor>::iterator itr = normList.begin(); itr != normList.end(); ++itr){

	NormFactor& norm = *itr;

        string varname;
        if(!prodNames.empty()) prodNames += ",";
        if(doRatio) {
          varname = norm.GetName() + "_" + channel;
        }
        else {
          varname=norm.GetName();
        }

	// GHL: Check that the NormFactor doesn't already exist
	//      (it may have been created as a function expression
	//       during preprocessing)
	std::stringstream range;
	range << "[" << norm.GetVal() << "," << norm.GetLow() << "," << norm.GetHigh() << "]";

	if( proto->obj(varname.c_str()) == NULL) {
	  cout << "making normFactor: " << norm.GetName() << endl;
	  // remove "doRatio" and name can be changed when ws gets imported to the combined model.
	  proto->factory((varname + range.str()).c_str());
	}

	if(norm.GetConst()) {
	  //	  proto->var(varname.c_str())->setConstant();
	  //	  cout <<"setting " << varname << " constant"<<endl;
	  cout << "WARNING: Const attribute to <NormFactor> tag is deprecated, will ignore." <<
	    " Instead, add \n\t<ParamSetting Const=\"True\">" << varname << "</ParamSetting>\n" <<
	    " to your top-level XML's <Measurment> entry" << endl;
	}
        prodNames+=varname;
        rangeNames.push_back(range.str());
	normFactorNames.push_back(varname);
      }

      overallNorm_times_sigmaEpsilon = sample.GetName() + "_" + channel + "_overallNorm_x_sigma_epsilon";
      proto->factory(("prod::" + overallNorm_times_sigmaEpsilon + "(" + prodNames + "," + sigmaEpsilon + ")").c_str());
    }

    unsigned int rangeIndex=0;
    for( vector<string>::iterator nit = normFactorNames.begin(); nit!=normFactorNames.end(); ++nit){
        if( count (normFactorNames.begin(), normFactorNames.end(), *nit) > 1 ){
	  cout <<"WARNING: <NormFactor Name =\""<<*nit<<"\"> is duplicated for <Sample Name=\"" 
	       << sample.GetName() << "\">, but only one factor will be included.  \n Instead, define something like" 
	       << "\n\t<Function Name=\""<<*nit<<"Squared\" Expresion=\""<<*nit<<"*"<<*nit<<"\" Var=\""<<*nit<<rangeNames.at(rangeIndex)
	       << "\"> \nin your top-level XML's <Measurment> entry and use <NormFactor Name=\""<<*nit<<"Squared\" in your channel XML file."<< endl;
	}
	++rangeIndex;
    }

    if(!overallNorm_times_sigmaEpsilon.empty())
      return overallNorm_times_sigmaEpsilon;
    else
      return sigmaEpsilon;
  }        

  void HistoToWorkspaceFactoryFast::AddEfficiencyTerms(RooWorkspace* proto, string prefix, 
						       string interpName,
						       std::vector<OverallSys>& systList, 
						       vector<string>& constraintTermNames, 
						       vector<string>& totSystTermNames) {

    // add variables for all the relative overall uncertainties we expect
    // range is set using defined macro (see top of the page)

    string range=string("[0,")+alpha_Low+","+alpha_High+"]";
    totSystTermNames.push_back(prefix);

    RooArgSet params(prefix.c_str());
    vector<double> lowVec, highVec;

    for(unsigned int i = 0; i < systList.size(); ++i) {

      OverallSys& sys = systList.at(i); 

      // add efficiency term
      RooRealVar* temp = (RooRealVar*) proto->var((prefix + sys.GetName()).c_str());
      if(!temp) {

	temp = (RooRealVar*) proto->factory((prefix + sys.GetName() + range).c_str());
	string command=("Gaussian::" + prefix + sys.GetName() + 
			"Constraint(" + prefix + sys.GetName() +
			",nom_" + prefix + sys.GetName() + "[0.,-10,10],1.)");

        cout << command << endl;
        constraintTermNames.push_back( proto->factory( command.c_str() )->GetName() );
	proto->var(("nom_" + prefix + sys.GetName()).c_str())->setConstant();
	const_cast<RooArgSet*>(proto->set("globalObservables"))->add(*proto->var(("nom_" + prefix + sys.GetName()).c_str()));	
      } 

      params.add(*temp);

      // add constraint in terms of bifrucated gauss with low/high as sigmas
      std::stringstream lowhigh;
      double low = sys.GetLow();
      double high = sys.GetHigh();
      lowVec.push_back(low);
      highVec.push_back(high);
      
    }

    if(systList.size() > 0){
      // this is epsilon(alpha_j), a piece-wise linear interpolation
      //      LinInterpVar interp( (interpName).c_str(), "", params, 1., lowVec, highVec);
      FlexibleInterpVar interp( (interpName).c_str(), "", params, 1., lowVec, highVec);
      interp.setAllInterpCodes(1); // changing default to piece-wise log interpolation from pice-wise linear
      proto->import(interp); // params have already been imported in first loop of this function
    } else{
      // some strange behavior if params,lowVec,highVec are empty.  
      //cout << "WARNING: No OverallSyst terms" << endl;
      RooConstVar interp( (interpName).c_str(), "", 1.);
      proto->import(interp); // params have already been imported in first loop of this function
    }
    
  }


  void  HistoToWorkspaceFactoryFast::MakeTotalExpected(RooWorkspace* proto, string totName, 
						       vector<string>& syst_x_expectedPrefixNames, 
                                                       vector<string>& normByNames){

    // for ith bin calculate totN_i =  lumi * sum_j expected_j * syst_j 
    string command;
    string coeffList="";
    string shapeList="";
    string prepend="";

    if (fObsNameVec.empty() && !fObsName.empty()) { fObsNameVec.push_back(fObsName); } 

    double binWidth(1.0);
    std::string obsNameVecStr;
    std::vector<std::string>::iterator itr = fObsNameVec.begin();
    for (; itr!=fObsNameVec.end(); ++itr) {
      std::string obsName = *itr;
      binWidth *= proto->var(obsName.c_str())->numBins()/(proto->var(obsName.c_str())->getMax() - proto->var(obsName.c_str())->getMin()) ; // MB: Note: requires fixed bin sizes
      if (obsNameVecStr.size()>0) { obsNameVecStr += "_"; }
      obsNameVecStr += obsName;
    }

    //vector<string>::iterator it=syst_x_expectedPrefixNames.begin();
    for(unsigned int j=0; j<syst_x_expectedPrefixNames.size();++j){
      std::stringstream str;
      str<<"_"<<j;
      // repatative, but we need one coeff for each term in the sum
      // maybe can be avoided if we don't use bin width as coefficient
      command=string(Form("binWidth_%s_%d[%f]",obsNameVecStr.c_str(),j,binWidth));     
      proto->factory(command.c_str());
      proto->var(Form("binWidth_%s_%d",obsNameVecStr.c_str(),j))->setConstant();
      coeffList+=prepend+"binWidth_"+obsNameVecStr+str.str();

      command="prod::L_x_"+syst_x_expectedPrefixNames.at(j)+"("+normByNames.at(j)+","+syst_x_expectedPrefixNames.at(j)+")";
      /*RooAbsReal* tempFunc =(RooAbsReal*) */
      proto->factory(command.c_str());
      shapeList+=prepend+"L_x_"+syst_x_expectedPrefixNames.at(j);
      prepend=",";
      
      // add to num int to product
      //      tempFunc->specialIntegratorConfig(kTRUE)->method1D().setLabel("RooBinIntegrator")  ;
      //      tempFunc->forceNumInt();

    }    

    proto->defineSet("coefList",coeffList.c_str());
    proto->defineSet("shapeList",shapeList.c_str());
    //    proto->factory(command.c_str());
    RooRealSumPdf tot(totName.c_str(),totName.c_str(),*proto->set("shapeList"),*proto->set("coefList"),kTRUE);
    tot.specialIntegratorConfig(kTRUE)->method1D().setLabel("RooBinIntegrator")  ;
    tot.specialIntegratorConfig(kTRUE)->method2D().setLabel("RooBinIntegrator")  ;
    tot.specialIntegratorConfig(kTRUE)->methodND().setLabel("RooBinIntegrator")  ;
    tot.forceNumInt();

    // for mixed generation in RooSimultaneous
    tot.setAttribute("GenerateBinned"); // for use with RooSimultaneous::generate in mixed mode
    //    tot.setAttribute("GenerateUnbinned"); // we don't want that

    /*
    // Use binned numeric integration
    int nbins = 0;
    if( fObsNameVec.size() == 1 ) {
      nbins = proto->var(fObsNameVec.at(0).c_str())->numBins();

      cout <<"num bis for RooRealSumPdf = "<<nbins <<endl;
      //int nbins = ((RooRealVar*) allVars.first())->numBins();
      tot.specialIntegratorConfig(kTRUE)->getConfigSection("RooBinIntegrator").setRealValue("numBins",nbins);
      tot.forceNumInt();
      
    } else {
      cout << "Bin Integrator only supports 1-d.  Will be slow." << std::endl;
    }
    */
    

    proto->import(tot);
    
  }

  void HistoToWorkspaceFactoryFast::AddPoissonTerms(RooWorkspace* proto, string prefix, string obsPrefix, string expPrefix, int lowBin, int highBin, 
           vector<string>& likelihoodTermNames){
    /////////////////////////////////
    // Relate observables to expected for each bin
    // later modify variable named expPrefix_i to be product of terms
    RooArgSet Pois(prefix.c_str());
    for(Int_t i=lowBin; i<highBin; ++i){
      std::stringstream str;
      str<<"_"<<i;
      //string command("Poisson::"+prefix+str.str()+"("+obsPrefix+str.str()+","+expPrefix+str.str()+")");
      string command("Poisson::"+prefix+str.str()+"("+obsPrefix+str.str()+","+expPrefix+str.str()+",1)");//for no rounding
      RooAbsArg* temp = (proto->factory( command.c_str() ) );

      // output
      cout << "Poisson Term " << command << endl;
      ((RooAbsPdf*) temp)->setEvalErrorLoggingMode(RooAbsReal::PrintErrors);
      //cout << temp << endl;

      likelihoodTermNames.push_back( temp->GetName() );
      Pois.add(* temp );
    }  
    proto->defineSet(prefix.c_str(),Pois); // add argset to workspace
  }

   void HistoToWorkspaceFactoryFast::SetObsToExpected(RooWorkspace* proto, string obsPrefix, string expPrefix, int lowBin, int highBin){ 
    /////////////////////////////////
    // set observed to expected
     TTree* tree = new TTree();
     Double_t* obsForTree = new Double_t[highBin-lowBin];
     RooArgList obsList("obsList");

     for(Int_t i=lowBin; i<highBin; ++i){
       std::stringstream str;
       str<<"_"<<i;
       RooRealVar* obs = (RooRealVar*) proto->var((obsPrefix+str.str()).c_str());
       cout << "expected number of events called: " << expPrefix << endl;
       RooAbsReal* exp = proto->function((expPrefix+str.str()).c_str());
       if(obs && exp){
         
         //proto->Print();
         obs->setVal(  exp->getVal() );
         cout << "setting obs"+str.str()+" to expected = " << exp->getVal() << " check: " << obs->getVal() << endl;
         
         // add entry to array and attach to tree
         obsForTree[i] = exp->getVal();
         tree->Branch((obsPrefix+str.str()).c_str(), obsForTree+i ,(obsPrefix+str.str()+"/D").c_str());
         obsList.add(*obs);
       }else{
         cout << "problem retrieving obs or exp " << obsPrefix+str.str() << obs << " " << expPrefix+str.str() << exp << endl;
       }
     }  
     tree->Fill();
     RooDataSet* data = new RooDataSet("expData","", tree, obsList); // one experiment

     delete tree;
     delete [] obsForTree;

     proto->import(*data);

  }

  //_____________________________________________________________
  void HistoToWorkspaceFactoryFast::EditSyst(RooWorkspace* proto, const char* pdfNameChar, 
					     map<string,double> gammaSyst, 
					     map<string,double> uniformSyst, 
					     map<string,double> logNormSyst, 
					     map<string,double> noSyst) {
    string pdfName(pdfNameChar);

    ModelConfig * combined_config = (ModelConfig *) proto->obj("ModelConfig");
    if( combined_config==NULL ) {
      std::cout << "Error: Failed to find object 'ModelConfig' in workspace: " 
		<< proto->GetName() << std::endl;
      throw hf_exc();
    }
    //    const RooArgSet * constrainedParams=combined_config->GetNuisanceParameters();
    //    RooArgSet temp(*constrainedParams);
    string edit="EDIT::newSimPdf("+pdfName+",";
    string editList;
    string lastPdf=pdfName;
    string precede="";
    unsigned int numReplacements = 0;
    unsigned int nskipped = 0;
    map<string,double>::iterator it;

    // add gamma terms and their constraints
    for(it=gammaSyst.begin(); it!=gammaSyst.end(); ++it) {
      //cout << "edit for " << it->first << "with rel uncert = " << it->second << endl;
      if(! proto->var(("alpha_"+it->first).c_str())){
	//cout << "systematic not there" << endl;
	nskipped++; 
	continue;
      }
      numReplacements++;      

      double relativeUncertainty = it->second;
      double scale = 1/sqrt((1+1/pow(relativeUncertainty,2)));
      
      // this is the Gamma PDF and in a form that doesn't have roundoff problems like the Poisson does
      proto->factory(Form("beta_%s[1,0,10]",it->first.c_str()));
      proto->factory(Form("y_%s[%f]",it->first.c_str(),1./pow(relativeUncertainty,2))) ;
      proto->factory(Form("theta_%s[%f]",it->first.c_str(),pow(relativeUncertainty,2))) ;
      proto->factory(Form("Gamma::beta_%sConstraint(beta_%s,sum::k_%s(y_%s,one[1]),theta_%s,zero[0])",
			  it->first.c_str(),
			  it->first.c_str(),
			  it->first.c_str(),
			  it->first.c_str(),
			  it->first.c_str())) ;

      /*
      // this has some problems because N in poisson is rounded to nearest integer     
      proto->factory(Form("Poisson::beta_%sConstraint(y_%s[%f],prod::taub_%s(taus_%s[%f],beta_%s[1,0,5]))",
			  it->first.c_str(),
			  it->first.c_str(),
			  1./pow(relativeUncertainty,2),
			  it->first.c_str(),
			    it->first.c_str(),
			  1./pow(relativeUncertainty,2),
			  it->first.c_str()
			  ) ) ;
      */
      //	combined->factory(Form("expr::alphaOfBeta('(beta-1)/%f',beta)",scale));
      //	combined->factory(Form("expr::alphaOfBeta_%s('(beta_%s-1)/%f',beta_%s)",it->first.c_str(),it->first.c_str(),scale,it->first.c_str()));
      proto->factory(Form("PolyVar::alphaOfBeta_%s(beta_%s,{%f,%f})",it->first.c_str(),it->first.c_str(),-1./scale,1./scale));
	
      // set beta const status to be same as alpha
      if(proto->var(Form("alpha_%s",it->first.c_str()))->isConstant()) {
	proto->var(Form("beta_%s",it->first.c_str()))->setConstant(true);
      }
      else {
	proto->var(Form("beta_%s",it->first.c_str()))->setConstant(false);
      }
      // set alpha const status to true
      //      proto->var(Form("alpha_%s",it->first.c_str()))->setConstant(true);

      // replace alphas with alphaOfBeta and replace constraints
      editList+=precede + "alpha_"+it->first+"Constraint=beta_" + it->first+ "Constraint";
      precede=",";
      editList+=precede + "alpha_"+it->first+"=alphaOfBeta_"+ it->first;

      /*
      if( proto->pdf(("alpha_"+it->first+"Constraint").c_str()) && proto->var(("alpha_"+it->first).c_str()) )
      cout << " checked they are there" << proto->pdf(("alpha_"+it->first+"Constraint").c_str()) << " " << proto->var(("alpha_"+it->first).c_str()) << endl;
      else
	cout << "NOT THERE" << endl;
      */

      // EDIT seems to die if the list of edits is too long.  So chunck them up.
      if(numReplacements%10 == 0 && numReplacements+nskipped!=gammaSyst.size()){
	edit="EDIT::"+lastPdf+"_("+lastPdf+","+editList+")";
	lastPdf+="_"; // append an underscore for the edit
	editList=""; // reset edit list
	precede="";
	cout << "Going to issue this edit command\n" << edit<< endl;
	proto->factory( edit.c_str() );
	RooAbsPdf* newOne = proto->pdf(lastPdf.c_str());
	if(!newOne)
	  cout << "\n\n ---------------------\n WARNING: failed to make EDIT\n\n" << endl;
	
      }
    }

    // add uniform terms and their constraints
    for(it=uniformSyst.begin(); it!=uniformSyst.end(); ++it) {
      cout << "edit for " << it->first << "with rel uncert = " << it->second << endl;
      if(! proto->var(("alpha_"+it->first).c_str())){
	cout << "systematic not there" << endl;
	nskipped++; 
	continue;
      }
      numReplacements++;      

      // this is the Uniform PDF
      proto->factory(Form("beta_%s[1,0,10]",it->first.c_str()));
      proto->factory(Form("Uniform::beta_%sConstraint(beta_%s)",it->first.c_str(),it->first.c_str()));
      proto->factory(Form("PolyVar::alphaOfBeta_%s(beta_%s,{-1,1})",it->first.c_str(),it->first.c_str()));
      
      // set beta const status to be same as alpha
      if(proto->var(Form("alpha_%s",it->first.c_str()))->isConstant())
	proto->var(Form("beta_%s",it->first.c_str()))->setConstant(true);
      else
	proto->var(Form("beta_%s",it->first.c_str()))->setConstant(false);
      // set alpha const status to true
      //      proto->var(Form("alpha_%s",it->first.c_str()))->setConstant(true);

      // replace alphas with alphaOfBeta and replace constraints
      cout <<         "alpha_"+it->first+"Constraint=beta_" + it->first+ "Constraint" << endl;
      editList+=precede + "alpha_"+it->first+"Constraint=beta_" + it->first+ "Constraint";
      precede=",";
      cout <<         "alpha_"+it->first+"=alphaOfBeta_"+ it->first << endl;
      editList+=precede + "alpha_"+it->first+"=alphaOfBeta_"+ it->first;

      if( proto->pdf(("alpha_"+it->first+"Constraint").c_str()) && proto->var(("alpha_"+it->first).c_str()) )
	cout << " checked they are there" << proto->pdf(("alpha_"+it->first+"Constraint").c_str()) << " " << proto->var(("alpha_"+it->first).c_str()) << endl;
      else
	cout << "NOT THERE" << endl;

      // EDIT seems to die if the list of edits is too long.  So chunck them up.
      if(numReplacements%10 == 0 && numReplacements+nskipped!=gammaSyst.size()){
	edit="EDIT::"+lastPdf+"_("+lastPdf+","+editList+")";
	lastPdf+="_"; // append an underscore for the edit
	editList=""; // reset edit list
	precede="";
	cout << edit<< endl;
	proto->factory( edit.c_str() );
	RooAbsPdf* newOne = proto->pdf(lastPdf.c_str());
	if(!newOne)
	  cout << "\n\n ---------------------\n WARNING: failed to make EDIT\n\n" << endl;
	
      }
    }

    /////////////////////////////////////////
    ////////////////////////////////////


    // add lognormal terms and their constraints
    for(it=logNormSyst.begin(); it!=logNormSyst.end(); ++it) {
      cout << "edit for " << it->first << "with rel uncert = " << it->second << endl;
      if(! proto->var(("alpha_"+it->first).c_str())){
	cout << "systematic not there" << endl;
	nskipped++; 
	continue;
      }
      numReplacements++;      

      double relativeUncertainty = it->second;
      double kappa = 1+relativeUncertainty;
      // when transforming beta -> alpha, need alpha=1 to be +1sigma value.
      // the P(beta>kappa*\hat(beta)) = 16%
      // and \hat(beta) is 1, thus
      double scale = relativeUncertainty;
      //double scale = kappa; 

      // this is the LogNormal
      proto->factory(Form("beta_%s[1,0,10]",it->first.c_str()));
      proto->factory(Form("kappa_%s[%f]",it->first.c_str(),kappa));
      proto->factory(Form("Lognormal::beta_%sConstraint(beta_%s,one[1],kappa_%s)",
			  it->first.c_str(),
			  it->first.c_str(),
			  it->first.c_str())) ;
      proto->factory(Form("PolyVar::alphaOfBeta_%s(beta_%s,{%f,%f})",it->first.c_str(),it->first.c_str(),-1./scale,1./scale));
      //      proto->factory(Form("PolyVar::alphaOfBeta_%s(beta_%s,{%f,%f})",it->first.c_str(),it->first.c_str(),-1.,1./scale));
      
      // set beta const status to be same as alpha
      if(proto->var(Form("alpha_%s",it->first.c_str()))->isConstant())
	proto->var(Form("beta_%s",it->first.c_str()))->setConstant(true);
      else
	proto->var(Form("beta_%s",it->first.c_str()))->setConstant(false);
      // set alpha const status to true
      //      proto->var(Form("alpha_%s",it->first.c_str()))->setConstant(true);

      // replace alphas with alphaOfBeta and replace constraints
      cout <<         "alpha_"+it->first+"Constraint=beta_" + it->first+ "Constraint" << endl;
      editList+=precede + "alpha_"+it->first+"Constraint=beta_" + it->first+ "Constraint";
      precede=",";
      cout <<         "alpha_"+it->first+"=alphaOfBeta_"+ it->first << endl;
      editList+=precede + "alpha_"+it->first+"=alphaOfBeta_"+ it->first;

      if( proto->pdf(("alpha_"+it->first+"Constraint").c_str()) && proto->var(("alpha_"+it->first).c_str()) )
	cout << " checked they are there" << proto->pdf(("alpha_"+it->first+"Constraint").c_str()) << " " << proto->var(("alpha_"+it->first).c_str()) << endl;
      else
	cout << "NOT THERE" << endl;

      // EDIT seems to die if the list of edits is too long.  So chunck them up.
      if(numReplacements%10 == 0 && numReplacements+nskipped!=gammaSyst.size()){
	edit="EDIT::"+lastPdf+"_("+lastPdf+","+editList+")";
	lastPdf+="_"; // append an underscore for the edit
	editList=""; // reset edit list
	precede="";
	cout << edit<< endl;
	proto->factory( edit.c_str() );
	RooAbsPdf* newOne = proto->pdf(lastPdf.c_str());
	if(!newOne)
	  cout << "\n\n ---------------------\n WARNING: failed to make EDIT\n\n" << endl;
	
      }
    }

    /////////////////////////////////////////

    // MB: remove a systematic constraint
    for(it=noSyst.begin(); it!=noSyst.end(); ++it) {

      cout << "remove constraint for parameter" << it->first << endl;
      if(! proto->var(("alpha_"+it->first).c_str()) || ! proto->pdf(("alpha_"+it->first+"Constraint").c_str()) ) {
	cout << "systematic not there" << endl;
	nskipped++; 
	continue;
      }
      numReplacements++;      

      // dummy replacement pdf
      if ( !proto->var("one") ) { proto->factory("one[1.0]"); }
      proto->var("one")->setConstant();

      // replace constraints
      cout << "alpha_"+it->first+"Constraint=one" << endl;
      editList+=precede + "alpha_"+it->first+"Constraint=one";
      precede=",";

      // EDIT seems to die if the list of edits is too long.  So chunck them up.
      if(numReplacements%10 == 0 && numReplacements+nskipped!=gammaSyst.size()){
	edit="EDIT::"+lastPdf+"_("+lastPdf+","+editList+")";
	lastPdf+="_"; // append an underscore for the edit
	editList=""; // reset edit list
	precede="";
	cout << edit << endl;
	proto->factory( edit.c_str() );
	RooAbsPdf* newOne = proto->pdf(lastPdf.c_str());
	if(!newOne) { cout << "\n\n ---------------------\n WARNING: failed to make EDIT\n\n" << endl; }
      }
    }

    /////////////////////////////////////////

    // commit last bunch of edits
    edit="EDIT::newSimPdf("+lastPdf+","+editList+")";
    cout << edit<< endl;
    proto->factory( edit.c_str() );
    //    proto->writeToFile(("results/model_"+fRowTitle+"_edited.root").c_str());
    RooAbsPdf* newOne = proto->pdf("newSimPdf");
    if(newOne){
      // newOne->graphVizTree(("results/"+pdfName+"_"+fRowTitle+"newSimPdf.dot").c_str());
      combined_config->SetPdf(*newOne);
    }
    else{
      cout << "\n\n ---------------------\n WARNING: failed to make EDIT\n\n" << endl;
    }
  }

  void HistoToWorkspaceFactoryFast::PrintCovarianceMatrix(RooFitResult* result, RooArgSet* params, string filename){
    // Change-> Now a static utility

    FILE* covFile = fopen ((filename).c_str(),"w"); 

    TIter iti = params->createIterator();
    TIter itj = params->createIterator();
    RooRealVar *myargi, *myargj; 
    fprintf(covFile," ") ;
    while ((myargi = (RooRealVar *)iti.Next())) { 
      if(myargi->isConstant()) continue;
      fprintf(covFile," & %s",  myargi->GetName());
    }
    fprintf(covFile,"\\\\ \\hline \n" );
    iti.Reset();
    while ((myargi = (RooRealVar *)iti.Next())) { 
      if(myargi->isConstant()) continue;
      fprintf(covFile,"%s", myargi->GetName());
      itj.Reset();
      while ((myargj = (RooRealVar *)itj.Next())) { 
        if(myargj->isConstant()) continue;
        cout << myargi->GetName() << "," << myargj->GetName();
        fprintf(covFile, " & %.2f", result->correlation(*myargi, *myargj));
      }
      cout << endl;
      fprintf(covFile, " \\\\\n");
    }
    fclose(covFile);
    
  }


  ///////////////////////////////////////////////
  RooWorkspace* HistoToWorkspaceFactoryFast::MakeSingleChannelWorkspace(Measurement& measurement, Channel& channel)
  {

    // Set these by hand inside the function
    vector<string> systToFix = measurement.GetConstantParams();
    bool doRatio=false;

    // to time the macro
    TStopwatch t;
    t.Start();
    //ES// string channel_name=summary[0].channel;
    string channel_name = channel.GetName();
    
    /// MB: reset observable names for each new channel.
    fObsNameVec.clear();

    /// MB: label observables x,y,z, depending on histogram dimensionality
    /// GHL: Give it the first sample's nominal histogram as a template
    ///      since the data histogram may not be present
    TH1* channel_hist_template = channel.GetSamples().at(0).GetHisto();
    if (fObsNameVec.empty()) { GuessObsNameVec(channel_hist_template); }

    for ( unsigned int idx=0; idx<fObsNameVec.size(); ++idx ) {
      fObsNameVec[idx] = "obs_" + fObsNameVec[idx] + "_" + channel_name ;
    }

    if (fObsNameVec.empty()) {
      fObsName= "obs_" + channel_name; // set name ov observable
      fObsNameVec.push_back( fObsName );
    }

    R__ASSERT( fObsNameVec.size()>=1 && fObsNameVec.size()<=3 );

    cout << "\n\n-------------------\nStarting to process " << channel_name << " channel with " << fObsNameVec.size() << " observables" << endl;

    //
    // our main workspace that we are using to construct the model
    //
    RooWorkspace* proto = new RooWorkspace(channel_name.c_str(), (channel_name+" workspace").c_str());
    ModelConfig * proto_config = new ModelConfig("ModelConfig", proto);
    proto_config->SetWorkspace(*proto);

    // preprocess functions
    vector<string>::iterator funcIter = fPreprocessFunctions.begin();
    for(;funcIter!= fPreprocessFunctions.end(); ++funcIter){
      cout <<"will preprocess this line: " << *funcIter <<endl;
      proto->factory(funcIter->c_str());
      proto->Print();
    }

    RooArgSet likelihoodTerms("likelihoodTerms"), constraintTerms("constraintTerms");
    vector<string> likelihoodTermNames, constraintTermNames, totSystTermNames, syst_x_expectedPrefixNames, normalizationNames;

    vector< pair<string,string> >   statNamePairs;
    vector< pair<TH1*,TH1*> >       statHistPairs; // <nominal, error>
    std::string                     statFuncName; // the name of the ParamHistFunc
    std::string                     statNodeName; // the name of the McStat Node
    Constraint::Type statConstraintType=Constraint::Gaussian;
    Double_t                        statRelErrorThreshold=0.0;

    string prefix, range;

    /////////////////////////////
    // shared parameters
    // this is ratio of lumi to nominal lumi.  We will include relative uncertainty in model
    std::stringstream lumiStr;
    // lumi range
    lumiStr<<"["<<fNomLumi<<",0,"<<10.*fNomLumi<<"]";
    proto->factory(("Lumi"+lumiStr.str()).c_str());
    cout << "lumi str = " << lumiStr.str() << endl;
    
    std::stringstream lumiErrorStr;
    lumiErrorStr << "nominalLumi["<<fNomLumi << ",0,"<<fNomLumi+10*fLumiError<<"]," << fLumiError ;
    proto->factory(("Gaussian::lumiConstraint(Lumi,"+lumiErrorStr.str()+")").c_str());
    proto->var("nominalLumi")->setConstant();
    proto->defineSet("globalObservables","nominalLumi");
    //likelihoodTermNames.push_back("lumiConstraint");
    constraintTermNames.push_back("lumiConstraint");
    cout << "lumi Error str = " << lumiErrorStr.str() << endl;
    
    //proto->factory((string("SigXsecOverSM[1.,0.5,1..8]").c_str()));
    ///////////////////////////////////
    // loop through estimates, add expectation, floating bin predictions, 
    // and terms that constrain floating to expectation via uncertainties
    // GHL: Loop over samples instead, which doesn't contain the data
    vector<Sample>::iterator it = channel.GetSamples().begin();
    for(; it!=channel.GetSamples().end(); ++it) {

      //ES// string overallSystName = it->name+"_"+it->channel+"_epsilon"; 
      Sample& sample = (*it);
      string overallSystName = sample.GetName() + "_" + channel_name + "_epsilon"; 

      string systSourcePrefix = "alpha_";

      // constraintTermNames and totSystTermNames are vectors that are passed
      // by reference and filled by this method
      AddEfficiencyTerms(proto,systSourcePrefix, overallSystName,
			 sample.GetOverallSysList(), constraintTermNames , totSystTermNames);    

      // GHL: Consider passing the NormFactor list instead of the entire sample
      overallSystName = AddNormFactor(proto, channel_name, overallSystName, sample, doRatio); 

      // Create the string for the object
      // that is added to the RooRealSumPdf
      // for this channel
      string syst_x_expectedPrefix = "";

      // get histogram
      //ES// TH1* nominal = it->nominal;
      TH1* nominal = sample.GetHisto();

      // MB : HACK no option to have both non-hist variations and hist variations ?
      // get histogram
      // GHL: Okay, this is going to be non-trivial.
      //      We will loop over histosys's, which contain both
      //      the low hist and the high hist together.

      // Logic:  
      //        - If we have no HistoSys's, do part A
      //        - else, if the histo syst's don't match, return (we ignore this case)
      //        - finally, we take the syst's and apply the linear interpolation w/ constraint

      if(sample.GetHistoSysList().size() == 0){

	// If no HistoSys
        cout << sample.GetName() + "_" + channel_name + " has no variation histograms " << endl;
        string expPrefix = sample.GetName() + "_" + channel_name; //+"_expN";
        syst_x_expectedPrefix = sample.GetName() + "_" + channel_name + "_overallSyst_x_Exp";

        ProcessExpectedHisto(sample.GetHisto(), proto, expPrefix, syst_x_expectedPrefix, 
			     overallSystName);
      } 
      else {
	// If there ARE HistoSys(s)
        string constraintPrefix = sample.GetName() + "_" + channel_name + "_Hist_alpha"; // name of source for variation
	syst_x_expectedPrefix = sample.GetName() + "_" + channel_name + "_overallSyst_x_HistSyst";
	// constraintTermNames are passed by reference and appended to,
	// overallSystName is a std::string for this sample

        LinInterpWithConstraint(proto, nominal, sample.GetHistoSysList(),
				constraintPrefix, syst_x_expectedPrefix, overallSystName, 
				constraintTermNames);
      }

      ////////////////////////////////////
      // Add StatErrors to this Channel //
      ////////////////////////////////////

      if( sample.GetStatError().GetActivate() ) {

	if( fObsNameVec.size() > 3 ) {
	  std::cout << "Cannot include Stat Error for histograms of more than 3 dimensions." << std::endl; 
	  throw hf_exc();
	} else {

	  // If we are using StatUncertainties, we multiply this object
	  // by the ParamHistFunc and then pass that to the
	  // RooRealSumPdf by appending it's name to the list

	  std::cout << "Sample: "     << sample.GetName()  << " to be included in Stat Error " 
		    << "for channel " << channel_name
		    << std::endl;

	  // Get the type of StatError constraint from the channel
	  Constraint::Type type = channel.GetStatErrorConfig().GetConstraintType();

	  // For now, convert it to an EstimateSummary constraint
	  // Will remove this intermediate step later
	  // EstimateSummary::ConstraintType:
	  statConstraintType = Constraint::Gaussian;
	  if( type == Constraint::Gaussian) {
	    std::cout << "Using Gaussian StatErrors" << std::endl;
	    statConstraintType = Constraint::Gaussian;
	  }
	  if( type == Constraint::Poisson ) {
	    std::cout << "Using Poisson StatErrors" << std::endl;
	    statConstraintType = Constraint::Poisson;
	  }

	  statRelErrorThreshold = channel.GetStatErrorConfig().GetRelErrorThreshold();


	  // First, get the uncertainty histogram
	  // and push it back to our vectors
	
	  // And get the Relative Error histogram for this sample, if there is one
	  TH1* statErrorHist = NULL;
	  if( sample.GetStatError().GetErrorHist() ) {
	    statErrorHist = (TH1*) sample.GetStatError().GetErrorHist()->Clone();
	  }

	  string UncertName  = syst_x_expectedPrefix + "_StatAbsolUncert";
	
	  if( statErrorHist == NULL ) {
	    // Make the absolute stat error
	    std::cout << "Making Statistical Uncertainty Hist for "
		      << " Channel: " << channel_name
		      << " Sample: "  << sample.GetName()
		      << std::endl;
	    statErrorHist = MakeAbsolUncertaintyHist( UncertName, nominal );
	  } else {
	    // We assume the (relative) error is provided.
	    // We must turn it into an absolute error
	    // using the nominal histogram
	    std::cout << "Using external histogram for Stat Errors for "
		      << " Channel: " << channel_name
		      << " Sample: "  << sample.GetName()
		      << std::endl;
	    std::cout << "Error Histogram: " << statErrorHist->GetName() << std::endl;
	    // Multiply the relative stat uncertainty by the
	    // nominal to get the overall stat uncertainty
	    statErrorHist->Multiply( nominal );
	    statErrorHist->SetName( UncertName.c_str() );
	  }
	
	  // Save the nominal and error hists
	  // for the building of constraint terms
	  statHistPairs.push_back( pair<TH1*,TH1*>(nominal,statErrorHist) );

	  // Next, try to get the flexible ParamHistFunc/
	  // or create it if it doesn't yet exist:
	  statFuncName = "mc_stat_" + channel_name;
	  ParamHistFunc* paramHist = (ParamHistFunc*) proto->function( statFuncName.c_str() );
	  if( paramHist == NULL ) {

	    // Get a RooArgSet of the observables:
	    // Names in the lsit fObsNameVec:
	    RooArgList observables;
	    std::vector<std::string>::iterator itr = fObsNameVec.begin();
	    for (int idx=0; itr!=fObsNameVec.end(); ++itr, ++idx ) {
	      observables.add( *proto->var(itr->c_str()) );
	    }
	  
	    //	    RooRealVar* var = (RooRealVar*) observables.first();
	  
	    // Create the list of terms to
	    // control the bin heights:
	    std::string ParamSetPrefix  = "gamma_stat_" + channel_name;
	    Double_t gammaMin = 0.0;
	    Double_t gammaMax = 10.0;
	    RooArgList statFactorParams = ParamHistFunc::createParamSet(*proto, ParamSetPrefix.c_str(), observables, gammaMin, gammaMax);

	    ParamHistFunc statUncertFunc(statFuncName.c_str(), statFuncName.c_str(), 
				       observables, statFactorParams );
	  
	    proto->import( statUncertFunc, RecycleConflictNodes() );

	    paramHist = (ParamHistFunc*) proto->function( statFuncName.c_str() );

	  } // END: If Statement: Create ParamHistFunc
	

	  // Create the node as a product
	  // of this function and the 
	  // expected value from MC
	  statNodeName = sample.GetName() + "_" + channel_name + "_overallSyst_x_StatUncert";
	
	  RooAbsReal* expFunc = (RooAbsReal*) proto->function( syst_x_expectedPrefix.c_str() );
	  RooProduct nodeWithMcStat(statNodeName.c_str(), statNodeName.c_str(),
				    RooArgSet(*paramHist, *expFunc) );
	
	  proto->import( nodeWithMcStat, RecycleConflictNodes() );
	
	  // Push back the final name of the node 
	  // to be used in the RooRealSumPdf 
	  // (node to be created later)
	  syst_x_expectedPrefix = nodeWithMcStat.GetName();

	}
      } // END: if DoMcStat
      

      ///////////////////////////////////////////
      // Create a ShapeFactor for this channel //
      ///////////////////////////////////////////

      // GHL: I belive this is logically what we want
      if( sample.GetShapeFactorList().size() > 0 ) {

	// // For now, only one shape factor per channel
	// if( sample.GetShapeFactorList().size() > 1 ) {
	//   std::cout << "Error: Only One Shape Factor currently supported" << std::endl;
	//   throw hf_exc();
	// }

	if( fObsNameVec.size() > 3 ) {
	  std::cout << "Cannot include Stat Error for histograms of more than 3 dimensions." << std::endl; 
	  // We should probably stop execution here, instead of being silent
	  throw hf_exc();
	} else {

	  std::cout << "Sample: "     << sample.GetName() << " in channel: " << channel_name
	  	    << " to be include a ShapeFactor."
	  	    << std::endl;
	  
	  std::vector<ParamHistFunc*> paramHistFuncList;
	  std::vector<std::string> shapeFactorNameList;

	  for(unsigned int i=0; i < sample.GetShapeFactorList().size(); ++i) {

	    ShapeFactor& shapeFactor = sample.GetShapeFactorList().at(i);

	    std::string funcName = channel_name + "_" + shapeFactor.GetName() + "_shapeFactor";
	    ParamHistFunc* paramHist = (ParamHistFunc*) proto->function( funcName.c_str() );
	    if( paramHist == NULL ) {
	      
	      RooArgList observables;
	      std::vector<std::string>::iterator itr = fObsNameVec.begin();
	      for (int idx=0; itr!=fObsNameVec.end(); ++itr, ++idx ) {
		observables.add( *proto->var(itr->c_str()) );
	      }
	      
	      //	    RooRealVar* var = (RooRealVar*) observables.first();
	      
	      // Create the Parameters
	      std::string funcParams = "gamma_" + shapeFactor.GetName();

	      // GHL: Again, we are putting hard ranges on the gamma's
	      //      We should change this to range from 0 to /inf
	      RooArgList shapeFactorParams = ParamHistFunc::createParamSet(*proto, funcParams.c_str(), observables, 0, 1000);
	      
	      // Create the Function
	      ParamHistFunc shapeFactorFunc( funcName.c_str(), funcName.c_str(),
					   observables, shapeFactorParams );
	      
	      proto->import( shapeFactorFunc, RecycleConflictNodes() );
	      paramHist = (ParamHistFunc*) proto->function( funcName.c_str() );
	  
	    } // End: Create ShapeFactor ParamHistFunc

	    paramHistFuncList.push_back(paramHist);
	    shapeFactorNameList.push_back(funcName);

	  } // End loop over ShapeFactor Systematics

	  // Now that we have the right ShapeFactor, 
	  // we multiply the expected function
	
	  //std::string shapeFactorNodeName = syst_x_expectedPrefix + "_x_" + funcName;
	  // Dynamically build the name as a long product
	  std::string shapeFactorNodeName = syst_x_expectedPrefix;
	  for( unsigned int i=0; i < shapeFactorNameList.size(); ++i) {
	    shapeFactorNodeName += "_x_" + shapeFactorNameList.at(i);
	  }

	  RooAbsReal* expFunc = (RooAbsReal*) proto->function( syst_x_expectedPrefix.c_str() );
	  RooArgSet nodesForProduct(*expFunc);
	  for( unsigned int i=0; i < paramHistFuncList.size(); ++i) {
	    nodesForProduct.add( *paramHistFuncList.at(i) );
	  }
	  //RooProduct nodeWithShapeFactor(shapeFactorNodeName.c_str(), 
	  //                               shapeFactorNodeName.c_str(),
	  //RooArgSet(*paramHist, *expFunc) );
	  RooProduct nodeWithShapeFactor(shapeFactorNodeName.c_str(), 
	                                 shapeFactorNodeName.c_str(),
					 nodesForProduct );
	
	  proto->import( nodeWithShapeFactor, RecycleConflictNodes() );

	  // Push back the final name of the node 
	  // to be used in the RooRealSumPdf 
	  // (node to be created later)
	  syst_x_expectedPrefix = nodeWithShapeFactor.GetName();
     
	}
      } // End: if ShapeFactorName!=""


      ////////////////////////////////////////
      // Create a ShapeSys for this channel //
      ////////////////////////////////////////

      if( sample.GetShapeSysList().size() != 0 ) {

	if( fObsNameVec.size() > 3 ) {
	  std::cout << "Cannot include Stat Error for histograms of more than 3 dimensions." << std::endl; 
	  throw hf_exc();
	} else {

	  // List of ShapeSys ParamHistFuncs
	  std::vector<string> ShapeSysNames;

	  for( unsigned int i = 0; i < sample.GetShapeSysList().size(); ++i) {
	  	    
	    // Create the ParamHistFunc's
	    // Create their constraint terms and add them
	    // to the list of constraint terms

	    // Create a single RooProduct over all of these
	    // paramHistFunc's
	    
	    // Send the name of that product to the RooRealSumPdf

	    RooStats::HistFactory::ShapeSys& shapeSys = sample.GetShapeSysList().at(i);

	    std::cout << "Sample: " << sample.GetName() << " in channel: " << channel_name
		      << " to include a ShapeSys." << std::endl;

	    std::string funcName = channel_name + "_" + shapeSys.GetName() + "_ShapeSys";
	    ShapeSysNames.push_back( funcName );
	    ParamHistFunc* paramHist = (ParamHistFunc*) proto->function( funcName.c_str() );
	    if( paramHist == NULL ) {

	      //std::string funcParams = "gamma_" + it->shapeFactorName;
	      //paramHist = CreateParamHistFunc( proto, fObsNameVec, funcParams, funcName );

	      RooArgList observables;
	      std::vector<std::string>::iterator itr = fObsNameVec.begin();
	      for(; itr!=fObsNameVec.end(); ++itr ) {
		observables.add( *proto->var(itr->c_str()) );
	      }

	      //	      RooRealVar* var = (RooRealVar*) observables.first();

	      // Create the Parameters
	      std::string funcParams = "gamma_" + shapeSys.GetName();
	      RooArgList shapeFactorParams = ParamHistFunc::createParamSet(*proto, funcParams.c_str(), 
									   observables, 0, 10);

	      // Create the Function
	      ParamHistFunc shapeFactorFunc( funcName.c_str(), funcName.c_str(),
					     observables, shapeFactorParams );

	      proto->import( shapeFactorFunc, RecycleConflictNodes() );
	      paramHist = (ParamHistFunc*) proto->function( funcName.c_str() );	      
	      
	    } // End: Create ShapeFactor ParamHistFunc

	    // Create the constraint terms and add
	    // them to the workspace (proto)
	    // as well as the list of constraint terms (constraintTermNames)
	    
	    // The syst should be a fractional error
	    TH1* shapeErrorHist = shapeSys.GetErrorHist();

	    Constraint::Type shapeConstraintType = Constraint::Gaussian;
	    Constraint::Type systype = shapeSys.GetConstraintType();
	    if( systype == Constraint::Gaussian) {
	      shapeConstraintType = Constraint::Gaussian;
	    }
	    if( systype == Constraint::Poisson ) {
	      shapeConstraintType = Constraint::Poisson;
	    }

	    Double_t minShapeUncertainty = 0.0;
	    RooArgList shapeConstraints = createStatConstraintTerms(proto, constraintTermNames, *paramHist, shapeErrorHist, 
								    shapeConstraintType, minShapeUncertainty);

	  } // End: Loop over ShapeSys vector in this EstimateSummary
	  
	  // Now that we have the list of ShapeSys ParamHistFunc names,
	  // we create the total RooProduct
	  // we multiply the expected functio
	  
	  std::string NodeName = syst_x_expectedPrefix;
	  RooArgList ShapeSysForNode;
	  RooAbsReal* expFunc = (RooAbsReal*) proto->function( syst_x_expectedPrefix.c_str() );
	  ShapeSysForNode.add( *expFunc );
	  for( unsigned int i = 0; i < ShapeSysNames.size(); ++i ) {
	    std::string ShapeSysName = ShapeSysNames.at(i);
	    ShapeSysForNode.add( *proto->function(ShapeSysName.c_str()) );
	    NodeName = NodeName + "_x_" + ShapeSysName;
	  }

	  // Create the name for this NEW Node
	  RooProduct nodeWithShapeFactor(NodeName.c_str(), NodeName.c_str(), ShapeSysForNode );
	  proto->import( nodeWithShapeFactor, RecycleConflictNodes() );

	  // Push back the final name of the node 
	  // to be used in the RooRealSumPdf 
	  // (node to be created later)
	  syst_x_expectedPrefix = nodeWithShapeFactor.GetName();

	} // End: NumObsVar == 1

      } // End: ShapeSysts Size != 0

      // Append the name of the "node"
      // that is to be summed with the
      // RooRealSumPdf
      syst_x_expectedPrefixNames.push_back(syst_x_expectedPrefix);

      // GHL: This was pretty confusing before,
      //      hopefully using the measurement directly
      //      will improve it
      if( sample.GetNormalizeByTheory() ) {
	normalizationNames.push_back( "Lumi" );
      }
      else {
	TString lumiParamString;
	lumiParamString += measurement.GetLumi();
	lumiParamString.ReplaceAll(' ', TString());
        normalizationNames.push_back(lumiParamString.Data());
      }

    } // END: Loop over EstimateSummaries
    //    proto->Print();

    // If a non-zero number of samples call for
    // Stat Uncertainties, create the statFactor functions

    if( statHistPairs.size() > 0 ) {
      
      // Create the histogram of (binwise)
      // stat uncertainties:
      TH1* fracStatError = MakeScaledUncertaintyHist( statNodeName + "_RelErr", statHistPairs ); 
      if( fracStatError == NULL ) {
	std::cout << "Error: Failed to make ScaledUncertaintyHist for: " << statNodeName << std::endl;
	throw hf_exc();
      }
      
      // Using this TH1* of fractinal stat errors, 
      // create a set of constraint terms:
      ParamHistFunc* chanStatUncertFunc = (ParamHistFunc*) proto->function( statFuncName.c_str() );
      std::cout << "About to create Constraint Terms from: " 
		<< chanStatUncertFunc->GetName()
		<< " params: " << chanStatUncertFunc->paramList()
		<< std::endl;

      // Get the constraint type and the
      // rel error threshold from the (last)
      // EstimateSummary looped over (but all
      // should be the same)

      RooArgList statConstraints = createStatConstraintTerms(proto, constraintTermNames, *chanStatUncertFunc, fracStatError, 
							     statConstraintType, statRelErrorThreshold);

    } // END: Loop over stat Hist Pairs
    
    
    ///////////////////////////////////
    // for ith bin calculate totN_i =  lumi * sum_j expected_j * syst_j 
    //MakeTotalExpected(proto,channel_name+"_model",channel_name,"Lumi",fLowBin,fHighBin, 
    //      syst_x_expectedPrefixNames, normalizationNames);
    MakeTotalExpected(proto, channel_name+"_model", //channel_name,"Lumi",fLowBin,fHighBin, 
		      syst_x_expectedPrefixNames, normalizationNames);
    likelihoodTermNames.push_back(channel_name+"_model");

    //////////////////////////////////////
    // fix specified parameters
    for(unsigned int i=0; i<systToFix.size(); ++i){
      RooRealVar* temp = proto->var((systToFix.at(i)).c_str());
      if(temp) {
	// set the parameter constant
	temp->setConstant();
	
	// remove the corresponding auxiliary observable from the global observables
	RooRealVar* auxMeas = NULL;
	if(systToFix.at(i)=="Lumi"){
	  auxMeas = proto->var("nominalLumi");
	} else {
	  auxMeas = proto->var(Form("nom_%s",temp->GetName()));
	}

	if(auxMeas){
	  const_cast<RooArgSet*>(proto->set("globalObservables"))->remove(*auxMeas);
	} else{
	  cout << "could not corresponding auxiliary measurement  " << Form("nom_%s",temp->GetName()) << endl;
	}
      } else {
	cout << "could not find variable " << systToFix.at(i) << " could not set it to constant" << endl;
      }
    }

    //////////////////////////////////////
    // final proto model
    for(unsigned int i=0; i<constraintTermNames.size(); ++i){
      RooAbsArg* proto_arg = (proto->arg(constraintTermNames[i].c_str()));
      if( proto_arg==NULL ) {
	std::cout << "Error: Cannot find arg set: " << constraintTermNames.at(i)
		  << " in workspace: " << proto->GetName() << std::endl;
	throw hf_exc();
      }
      constraintTerms.add( *proto_arg );
      //  constraintTerms.add(* proto_arg(proto->arg(constraintTermNames[i].c_str())) );
    }
    for(unsigned int i=0; i<likelihoodTermNames.size(); ++i){
      RooAbsArg* proto_arg = (proto->arg(likelihoodTermNames[i].c_str())); 
      if( proto_arg==NULL ) {
	std::cout << "Error: Cannot find arg set: " << constraintTermNames.at(i)
		  << " in workspace: " << proto->GetName() << std::endl;
	throw hf_exc();
      }
      likelihoodTerms.add( *proto_arg );
    }
    proto->defineSet("constraintTerms",constraintTerms);
    proto->defineSet("likelihoodTerms",likelihoodTerms);
    //  proto->Print();

    // list of observables
    RooArgList observables;
    std::string observablesStr;

    std::vector<std::string>::iterator itr = fObsNameVec.begin();
    for(; itr!=fObsNameVec.end(); ++itr ) {
      observables.add( *proto->var(itr->c_str()) );
      if (!observablesStr.empty()) { observablesStr += ","; }
      observablesStr += *itr;
    }

    // We create two sets, one for backwards compatability
    // The other to make a consistent naming convention
    // between individual channels and the combined workspace
    proto->defineSet("observables", Form("%s",observablesStr.c_str()));
    proto->defineSet("observablesSet", Form("%s",observablesStr.c_str()));
    
    // Create the ParamHistFunc
    // after observables have been made
    cout <<"-----------------------------------------"<<endl;
    cout <<"import model into workspace" << endl;

    RooProdPdf* model = new RooProdPdf(("model_"+channel_name).c_str(),    // MB : have changed this into conditional pdf. Much faster for toys!
               "product of Poissons accross bins for a single channel",
	       constraintTerms, Conditional(likelihoodTerms,observables));  //likelihoodTerms);
    proto->import(*model,RecycleConflictNodes());

    proto_config->SetPdf(*model);
    proto_config->SetObservables(observables);
    proto_config->SetGlobalObservables(*proto->set("globalObservables"));
    //    proto->writeToFile(("results/model_"+channel+".root").c_str());
    // fill out nuisance parameters in model config
    //    proto_config->GuessObsAndNuisance(*proto->data("asimovData"));
    proto->import(*proto_config,proto_config->GetName());
    proto->importClassCode();

    ///////////////////////////
    // make data sets
      // THis works and is natural, but the memory size of the simultaneous dataset grows exponentially with channels
    const char* weightName="weightVar";
    proto->factory(Form("%s[0,-1e10,1e10]",weightName));
    proto->defineSet("obsAndWeight",Form("%s,%s",weightName,observablesStr.c_str()));

    /* Old code for generating the asimov
       Asimov generation is now done later...
       
    RooAbsData* asimov_data = model->generateBinned(observables,ExpectedData());

    /// Asimov dataset
    RooDataSet* asimovDataUnbinned = new RooDataSet("asimovData","",*proto->set("obsAndWeight"),weightName);
    for(int i=0; i<asimov_data->numEntries(); ++i){
      asimov_data->get(i)->Print("v");
      //cout << "GREPME : " << i << " " << data->weight() <<endl;
      asimovDataUnbinned->add( *asimov_data->get(i), asimov_data->weight() );
    }
    proto->import(*asimovDataUnbinned);
    */

    // New Asimov Generation: Use the code in the Asymptotic calculator 
    // Need to get the ModelConfig...
    RooDataSet* asimov_dataset = (RooDataSet*) AsymptoticCalculator::GenerateAsimovData(*model, observables);
    proto->import(*asimov_dataset, Rename("asimovData"));

    // GHL: Determine to use data if the hist isn't 'NULL'
    if(channel.GetData().GetHisto() != NULL) { 

      Data& data = channel.GetData();
      TH1* mnominal = data.GetHisto(); 
      if( !mnominal ) {
	std::cout << "Error: Data histogram for channel: " << channel.GetName()
		  << " is NULL" << std::endl;
	throw hf_exc();
      }

      // THis works and is natural, but the memory size of the simultaneous dataset grows exponentially with channels
      RooDataSet* obsDataUnbinned = new RooDataSet("obsData","",*proto->set("obsAndWeight"),weightName);


      ConfigureHistFactoryDataset( obsDataUnbinned, mnominal, 
				   proto, fObsNameVec );
      
      /*
      //ES// TH1* mnominal = summary.at(0).nominal;
      TH1* mnominal = data.GetHisto(); 
      TAxis* ax = mnominal->GetXaxis(); 
      TAxis* ay = mnominal->GetYaxis(); 
      TAxis* az = mnominal->GetZaxis(); 	

      for (int i=1; i<=ax->GetNbins(); ++i) { // 1 or more dimension
	Double_t xval = ax->GetBinCenter(i);
	proto->var( fObsNameVec[0].c_str() )->setVal( xval );
	if        (fObsNameVec.size()==1) {
	  Double_t fval = mnominal->GetBinContent(i);
	  obsDataUnbinned->add( *proto->set("obsAndWeight"), fval );
	} else { // 2 or more dimensions
	  for (int j=1; j<=ay->GetNbins(); ++j) {
	    Double_t yval = ay->GetBinCenter(j);
	    proto->var( fObsNameVec[1].c_str() )->setVal( yval );
	    if (fObsNameVec.size()==2) { 
	      Double_t fval = mnominal->GetBinContent(i,j);
	      obsDataUnbinned->add( *proto->set("obsAndWeight"), fval );
	    } else { // 3 dimensions 
	      for (int k=1; k<=az->GetNbins(); ++k) {
		Double_t zval = az->GetBinCenter(k);
		proto->var( fObsNameVec[2].c_str() )->setVal( zval );
		Double_t fval = mnominal->GetBinContent(i,j,k);
		obsDataUnbinned->add( *proto->set("obsAndWeight"), fval );
	      }
	    }
	  }
	}
      }
      */

      proto->import(*obsDataUnbinned);
    } // End: Has non-null 'data' entry

    
    for(unsigned int i=0; i < channel.GetAdditionalData().size(); ++i) {
      
      Data& data = channel.GetAdditionalData().at(i);
      std::string dataName = data.GetName();
      TH1* mnominal = data.GetHisto(); 
      if( !mnominal ) {
	std::cout << "Error: Additional Data histogram for channel: " << channel.GetName()
		  << " with name: " << dataName << " is NULL" << std::endl;
	throw hf_exc();
      }

      // THis works and is natural, but the memory size of the simultaneous dataset grows exponentially with channels
      RooDataSet* obsDataUnbinned = new RooDataSet(dataName.c_str(), dataName.c_str(),
						   *proto->set("obsAndWeight"), weightName);
      
      ConfigureHistFactoryDataset( obsDataUnbinned, mnominal, 
				   proto, fObsNameVec );
      
      /*
      //ES// TH1* mnominal = summary.at(0).nominal;
      TH1* mnominal = data.GetHisto(); 
      TAxis* ax = mnominal->GetXaxis(); 
      TAxis* ay = mnominal->GetYaxis(); 
      TAxis* az = mnominal->GetZaxis(); 	

      for (int i=1; i<=ax->GetNbins(); ++i) { // 1 or more dimension
	Double_t xval = ax->GetBinCenter(i);
	proto->var( fObsNameVec[0].c_str() )->setVal( xval );
	if        (fObsNameVec.size()==1) {
	  Double_t fval = mnominal->GetBinContent(i);
	  obsDataUnbinned->add( *proto->set("obsAndWeight"), fval );
	} else { // 2 or more dimensions
	  for (int j=1; j<=ay->GetNbins(); ++j) {
	    Double_t yval = ay->GetBinCenter(j);
	    proto->var( fObsNameVec[1].c_str() )->setVal( yval );
	    if (fObsNameVec.size()==2) { 
	      Double_t fval = mnominal->GetBinContent(i,j);
	      obsDataUnbinned->add( *proto->set("obsAndWeight"), fval );
	    } else { // 3 dimensions 
	      for (int k=1; k<=az->GetNbins(); ++k) {
		Double_t zval = az->GetBinCenter(k);
		proto->var( fObsNameVec[2].c_str() )->setVal( zval );
		Double_t fval = mnominal->GetBinContent(i,j,k);
		obsDataUnbinned->add( *proto->set("obsAndWeight"), fval );
	      }
	    }
	  }
	}
      }
      */

      proto->import(*obsDataUnbinned);
    } // End: Has non-null 'data' entry

    proto->Print();
    return proto;
  }


  void HistoToWorkspaceFactoryFast::ConfigureHistFactoryDataset( RooDataSet* obsDataUnbinned, 
								 TH1* mnominal, 
								 RooWorkspace* proto,
								 std::vector<std::string> ObsNameVec) {

    // Take a RooDataSet and fill it with the entries
    // from a TH1*, using the observable names to
    // determine the columns
    
    //ES// TH1* mnominal = summary.at(0).nominal;
    // TH1* mnominal = data.GetHisto(); 
    TAxis* ax = mnominal->GetXaxis(); 
    TAxis* ay = mnominal->GetYaxis(); 
    TAxis* az = mnominal->GetZaxis(); 	

    for (int i=1; i<=ax->GetNbins(); ++i) { // 1 or more dimension

      Double_t xval = ax->GetBinCenter(i);
      proto->var( ObsNameVec[0].c_str() )->setVal( xval );

      if(ObsNameVec.size()==1) {
	Double_t fval = mnominal->GetBinContent(i);
	obsDataUnbinned->add( *proto->set("obsAndWeight"), fval );
      } else { // 2 or more dimensions

	for(int j=1; j<=ay->GetNbins(); ++j) {
	  Double_t yval = ay->GetBinCenter(j);
	  proto->var( ObsNameVec[1].c_str() )->setVal( yval );

	  if(ObsNameVec.size()==2) { 
	    Double_t fval = mnominal->GetBinContent(i,j);
	    obsDataUnbinned->add( *proto->set("obsAndWeight"), fval );
	  } else { // 3 dimensions 

	    for(int k=1; k<=az->GetNbins(); ++k) {
	      Double_t zval = az->GetBinCenter(k);
	      proto->var( ObsNameVec[2].c_str() )->setVal( zval );
	      Double_t fval = mnominal->GetBinContent(i,j,k);
	      obsDataUnbinned->add( *proto->set("obsAndWeight"), fval );
	    }
	  }
	}
      }
    }
  }

  void HistoToWorkspaceFactoryFast::GuessObsNameVec(TH1* hist)
  {
    fObsNameVec.clear();

    // determine histogram dimensionality 
    unsigned int histndim(1);
    std::string classname = hist->ClassName();
    if      (classname.find("TH1")==0) { histndim=1; }
    else if (classname.find("TH2")==0) { histndim=2; }
    else if (classname.find("TH3")==0) { histndim=3; }

    for ( unsigned int idx=0; idx<histndim; ++idx ) {
      if (idx==0) { fObsNameVec.push_back("x"); }
      if (idx==1) { fObsNameVec.push_back("y"); }
      if (idx==2) { fObsNameVec.push_back("z"); }
    }
  }


  RooWorkspace* HistoToWorkspaceFactoryFast::MakeCombinedModel(vector<string> ch_names, vector<RooWorkspace*> chs)
  {

    //
    /// These things were used for debugging. Maybe useful in the future
    //

    map<string, RooAbsPdf*> pdfMap;
    vector<RooAbsPdf*> models;
    stringstream ss;

    RooArgList obsList;
    for(unsigned int i = 0; i< ch_names.size(); ++i){
      ModelConfig * config = (ModelConfig *) chs[i]->obj("ModelConfig");
      obsList.add(*config->GetObservables());
    }
    cout <<"full list of observables:"<<endl;
    obsList.Print();

    RooArgSet globalObs;
    for(unsigned int i = 0; i< ch_names.size(); ++i){
      string channel_name=ch_names[i];

      if (ss.str().empty()) ss << channel_name ;
      else ss << ',' << channel_name ;
      RooWorkspace * ch=chs[i];
      
      RooAbsPdf* model = ch->pdf(("model_"+channel_name).c_str());
      if(!model) cout <<"failed to find model for channel"<<endl;
      //      cout << "int = " << model->createIntegral(*obsN)->getVal() << endl;;
      models.push_back(model);
      globalObs.add(*ch->set("globalObservables"));

      //      constrainedParams->add( * ch->set("constrainedParams") );
      pdfMap[channel_name]=model;
    }
    //constrainedParams->Print();

    cout << "\n\n------------------\n Entering combination" << endl;
    RooWorkspace* combined = new RooWorkspace("combined");
    //    RooWorkspace* combined = chs[0];
    

    RooCategory* channelCat = (RooCategory*) combined->factory(("channelCat["+ss.str()+"]").c_str());
    RooSimultaneous * simPdf= new RooSimultaneous("simPdf","",pdfMap, *channelCat);
    ModelConfig * combined_config = new ModelConfig("ModelConfig", combined);
    combined_config->SetWorkspace(*combined);
    //    combined_config->SetNuisanceParameters(*constrainedParams);

    combined->import(globalObs);
    combined->defineSet("globalObservables",globalObs);
    combined_config->SetGlobalObservables(*combined->set("globalObservables"));
    

    ////////////////////////////////////////////
    // Make toy simultaneous dataset
    cout <<"-----------------------------------------"<<endl;
    cout << "create toy data for " << ss.str() << endl;
    

    // now with weighted datasets
    // First Asimov
    //RooDataSet * simData=NULL;
    combined->factory("weightVar[0,-1e10,1e10]");
    obsList.add(*combined->var("weightVar"));

    // Loop over channels and create the asimov
    /*
    for(unsigned int i = 0; i< ch_names.size(); ++i){
      cout << "merging data for channel " << ch_names[i].c_str() << endl;
      RooDataSet * tempData=new RooDataSet(ch_names[i].c_str(),"", obsList, Index(*channelCat),
					   WeightVar("weightVar"),
					  Import(ch_names[i].c_str(),*(RooDataSet*)chs[i]->data("asimovData")));
      if(simData){
	simData->append(*tempData);
      delete tempData;
      }else{
	simData = tempData;
      }
    }
    
    if (simData) combined->import(*simData,Rename("asimovData"));
    */
    RooDataSet* asimov_combined = (RooDataSet*) AsymptoticCalculator::GenerateAsimovData(*simPdf, 
											 obsList);
    if( asimov_combined ) {
      combined->import( *asimov_combined, Rename("asimovData"));
    }
    else {
      std::cout << "Error: Failed to create combined asimov dataset" << std::endl;
      throw hf_exc();
    }

    // Now merge the observable datasets across the channels
    if(chs[0]->data("obsData") != NULL) { 
      MergeDataSets(combined, chs, ch_names, "obsData", obsList, channelCat);
    }

    /*
    if(chs[0]->data("obsData") != NULL){
      RooDataSet * simData=NULL;
      //simData=NULL;

      // Loop through channels, get their individual datasets,
      // and add them to the combined dataset
      for(unsigned int i = 0; i< ch_names.size(); ++i){
	cout << "merging data for channel " << ch_names[i].c_str() << endl;

	RooDataSet* obsDataInChannel = (RooDataSet*) chs[i]->data("obsData");
	RooDataSet * tempData = new RooDataSet(ch_names[i].c_str(),"", obsList, Index(*channelCat),
					       WeightVar("weightVar"),
					       Import(ch_names[i].c_str(),*obsDataInChannel)); 
	// *(RooDataSet*) chs[i]->data("obsData")));
	if(simData) {
	  simData->append(*tempData);
	  delete tempData;
	}
	else {
	  simData = tempData;
	}
      } // End Loop Over Channels
      
      // Check that we successfully created the dataset
      // and import it into the workspace
      if(simData) {
	combined->import(*simData, Rename("obsData"));
      }
      else {
	std::cout << "Error: Unable to merge observable datasets" << std::endl;
	throw hf_exc();
      }

    } // End 'if' on data != NULL
    */

    // Now, create any additional Asimov datasets that
    // are configured in the measurement


    //    obsList.Print();
    //    combined->import(obsList);
    //    combined->Print();

    obsList.add(*channelCat);
    combined->defineSet("observables",obsList);
    combined_config->SetObservables(*combined->set("observables"));

    combined->Print();

    cout << "\n\n----------------\n Importing combined model" << endl;
    combined->import(*simPdf,RecycleConflictNodes());
    //combined->import(*simPdf, RenameVariable("SigXsecOverSM","SigXsecOverSM_comb"));
    // cout << "check pointer " << simPdf << endl;
    //    cout << "check val " << simPdf->getVal() << endl;

    std::map< std::string, double>::iterator param_itr = fParamValues.begin();
    for( ; param_itr != fParamValues.end(); ++param_itr ){
      // make sure they are fixed
      std::string paramName = param_itr->first;
      double paramVal = param_itr->second;
      
      RooRealVar* temp = combined->var( paramName.c_str() );
      if(temp) {
        temp->setVal( paramVal );
        cout <<"setting " << paramName << " to the value: " << paramVal <<  endl;
      } else 
	cout << "could not find variable " << paramName << " could not set its value" << endl;
    }


    for(unsigned int i=0; i<fSystToFix.size(); ++i){
      // make sure they are fixed
      RooRealVar* temp = combined->var((fSystToFix.at(i)).c_str());
      if(temp) {
        temp->setConstant();
        cout <<"setting " << fSystToFix.at(i) << " constant" << endl;
      } else 
	cout << "could not find variable " << fSystToFix.at(i) << " could not set it to constant" << endl;
    }

    ///
    /// writing out the model in graphViz
    /// 
    //    RooAbsPdf* customized=combined->pdf("simPdf"); 
    //combined_config->SetPdf(*customized);
    combined_config->SetPdf(*simPdf);
    //    combined_config->GuessObsAndNuisance(*simData);
    //    customized->graphVizTree(("results/"+fResultsPrefixStr.str()+"_simul.dot").c_str());
    combined->import(*combined_config,combined_config->GetName());
    combined->importClassCode();
    //    combined->writeToFile("results/model_combined.root");

    return combined;
  }


  RooDataSet* HistoToWorkspaceFactoryFast::MergeDataSets(RooWorkspace* combined,
							 std::vector<RooWorkspace*> wspace_vec, 
							 std::vector<std::string> channel_names, 
							 std::string dataSetName,
							 RooArgList obsList,
							 RooCategory* channelCat) {

    // Create the total dataset
    RooDataSet* simData=NULL;

    // Loop through channels, get their individual datasets,
    // and add them to the combined dataset
    for(unsigned int i = 0; i< channel_names.size(); ++i){

      // Grab the dataset for the existing channel
      std::cout << "Merging data for channel " << channel_names[i].c_str() << std::endl;
      RooDataSet* obsDataInChannel = (RooDataSet*) wspace_vec[i]->data(dataSetName.c_str());
      if( !obsDataInChannel ) {
	std::cout << "Error: Can't find DataSet: " << dataSetName
		  << " in channel: " << channel_names.at(i)
		  << std::endl;
	throw hf_exc();
      }

      // Create the new Dataset
      RooDataSet * tempData = new RooDataSet(channel_names[i].c_str(),"", 
					     obsList, Index(*channelCat),
					     WeightVar("weightVar"),
					     Import(channel_names[i].c_str(),*obsDataInChannel)); 
      if(simData) {
	simData->append(*tempData);
	delete tempData;
      }
      else {
	simData = tempData;
      }
    } // End Loop Over Channels
      
    // Check that we successfully created the dataset
    // and import it into the workspace
    if(simData) {
      combined->import(*simData, Rename(dataSetName.c_str()));
    }
    else {
      std::cout << "Error: Unable to merge observable datasets" << std::endl;
      throw hf_exc();
    }

    return simData;

  }
    

  TH1* HistoToWorkspaceFactoryFast::MakeAbsolUncertaintyHist( const std::string& Name, const TH1* Nominal ) {

    // Take a nominal TH1* and create
    // a TH1 representing the binwise
    // errors (taken from the nominal TH1)

    TH1* ErrorHist = (TH1*) Nominal->Clone( Name.c_str() );
    ErrorHist->Reset();
    
    Int_t numBins   = Nominal->GetNbinsX()*Nominal->GetNbinsY()*Nominal->GetNbinsZ();
    Int_t binNumber = 0;

    // Loop over bins
    for( Int_t i_bin = 0; i_bin < numBins; ++i_bin) {

      binNumber++;
      // Ignore underflow / overflow
      while( Nominal->IsBinUnderflow(binNumber) || Nominal->IsBinOverflow(binNumber) ){
	binNumber++;
      }

      Double_t histError = Nominal->GetBinError( binNumber );
    
      // Check that histError != NAN
      if( histError != histError ) {
	std::cout << "Warning: In histogram " << Nominal->GetName()
		  << " bin error for bin " << i_bin
		  << " is NAN.  Not using Error!!!"
		  << std::endl;
	throw hf_exc();
	//histError = sqrt( histContent );
	//histError = 0;
      }
    
      // Check that histError ! < 0
      if( histError < 0  ) {
	std::cout << "Warning: In histogram " << Nominal->GetName()
		  << " bin error for bin " << binNumber
		  << " is < 0.  Setting Error to 0"
		  << std::endl;
	//histError = sqrt( histContent );
	histError = 0;
      }

      ErrorHist->SetBinContent( binNumber, histError );

    }

    return ErrorHist;
  
  }
  
  TH1* HistoToWorkspaceFactoryFast::MakeScaledUncertaintyHist( const std::string& Name, std::vector< std::pair<TH1*, TH1*> > HistVec ) {

    // Take a list of < nominal, absolError > TH1* pairs
    // and construct a single histogram representing the 
    // total fractional error as:

    // UncertInQuad(bin i) = Sum: absolUncert*absolUncert
    // Total(bin i)        = Sum: Value
    //
    // TotalFracError(bin i) = Sqrt( UncertInQuad(i) ) / TotalBin(i)
    

    unsigned int numHists = HistVec.size();
    
    if( numHists == 0 ) {
      std::cout << "Warning: Empty Hist Vector, cannot create total uncertainty" << std::endl;
      return NULL;
    }
    
    TH1* HistTemplate = HistVec.at(0).first;
    Int_t numBins = HistTemplate->GetNbinsX()*HistTemplate->GetNbinsY()*HistTemplate->GetNbinsZ();

  // Check that all histograms
  // have the same bins
  for( unsigned int i = 0; i < HistVec.size(); ++i ) {
    
    TH1* nominal = HistVec.at(i).first;
    TH1* error   = HistVec.at(i).second;
    
    if( nominal->GetNbinsX()*nominal->GetNbinsY()*nominal->GetNbinsZ() != numBins ) {
      std::cout << "Error: Provided hists have unequal bins" << std::endl;
      return NULL;
    }
    if( error->GetNbinsX()*error->GetNbinsY()*error->GetNbinsZ() != numBins ) {
      std::cout << "Error: Provided hists have unequal bins" << std::endl;
      return NULL;
    }
  }

  std::vector<double> TotalBinContent( numBins, 0.0);
  std::vector<double> HistErrorsSqr( numBins, 0.0);

  Int_t binNumber = 0;

  // Loop over bins
  for( Int_t i_bins = 0; i_bins < numBins; ++i_bins) {
    
    binNumber++;
    while( HistTemplate->IsBinUnderflow(binNumber) || HistTemplate->IsBinOverflow(binNumber) ){
      binNumber++;
    }
    
    for( unsigned int i_hist = 0; i_hist < numHists; ++i_hist ) {
      
      TH1* nominal = HistVec.at(i_hist).first;
      TH1* error   = HistVec.at(i_hist).second;

      //Int_t binNumber = i_bins + 1;

      Double_t histValue  = nominal->GetBinContent( binNumber );
      Double_t histError  = error->GetBinContent( binNumber );
      /*
      std::cout << " Getting Bin content for Stat Uncertainty"
		<< " Nom name: " << nominal->GetName()
		<< " Err name: " << error->GetName()
		<< " HistNumber: " << i_hist << " bin: " << binNumber
		<< " Value: " << histValue << " Error: " << histError
		<< std::endl;
      */

      if( histError != histError ) {
	std::cout << "Warning: In histogram " << error->GetName()
		  << " bin error for bin " << binNumber
		  << " is NAN.  Not using error!!"
		  << std::endl;
	throw hf_exc();
	//histError = 0;
      }
      
      TotalBinContent.at(i_bins) += histValue;
      HistErrorsSqr.at(i_bins)   += histError*histError; // Add in quadrature

    }
  }

  binNumber = 0;

  // Creat the output histogram
  TH1* ErrorHist = (TH1*) HistTemplate->Clone( Name.c_str() );
  ErrorHist->Reset();

  // Fill the output histogram
  for( Int_t i = 0; i < numBins; ++i) {

    //    Int_t binNumber = i + 1;
    binNumber++;
    while( ErrorHist->IsBinUnderflow(binNumber) || ErrorHist->IsBinOverflow(binNumber) ){
      binNumber++;
    }

    Double_t ErrorsSqr = HistErrorsSqr.at(i);
    Double_t TotalVal  = TotalBinContent.at(i);

    if( TotalVal <= 0 ) {
      std::cout << "Warning: Sum of histograms for bin: " << binNumber
		<< " is <= 0.  Setting error to 0"
		<< std::endl;

      ErrorHist->SetBinContent( binNumber, 0.0 );
      continue;
    }

    Double_t RelativeError = sqrt(ErrorsSqr) / TotalVal;

    // If we otherwise get a NAN
    // it's an error
    if( RelativeError != RelativeError ) {
      std::cout << "Error: bin " << i << " error is NAN" << std::endl;
      std::cout << " HistErrorsSqr: " << ErrorsSqr
		<< " TotalVal: " << TotalVal
		<< std::endl;
      throw hf_exc();
    }

    // 0th entry in vector is
    // the 1st bin in TH1 
    // (we ignore underflow)

    ErrorHist->SetBinContent( binNumber, RelativeError );
    
    std::cout << "Making Total Uncertainty for bin " << binNumber
	      << " Error = " << sqrt(ErrorsSqr)
	      << " Val = " << TotalVal
	      << " RelativeError = " << RelativeError
	      << std::endl;

  }

  return ErrorHist;

}



  RooArgList HistoToWorkspaceFactoryFast::createStatConstraintTerms( RooWorkspace* proto, vector<string>& constraintTermNames,
								     ParamHistFunc& paramHist, TH1* uncertHist, 
								     Constraint::Type type, Double_t minSigma ) {


  // Take a RooArgList of RooAbsReal's and
  // create N constraint terms (one for
  // each gamma) whose relative uncertainty
  // is the value of the ith RooAbsReal
  //
  // The integer "type" controls the type
  // of constraint term:
  //
  // type == 0 : NONE
  // type == 1 : Gaussian
  // type == 2 : Poisson
  // type == 3 : LogNormal

  RooArgList ConstraintTerms;

  RooArgList paramSet = paramHist.paramList();


  // Must get the full size of the TH1
  // (No direct method to do this...)
  Int_t numBins   = uncertHist->GetNbinsX()*uncertHist->GetNbinsY()*uncertHist->GetNbinsZ();
  Int_t numParams = paramSet.getSize();
  //  Int_t numBins   = uncertHist->GetNbinsX()*uncertHist->GetNbinsY()*uncertHist->GetNbinsZ();

  // Check that there are N elements
  // in the RooArgList
  if( numBins != numParams ) {
    std::cout << "createStatConstraintTerms: bad number of bins" << std::endl;
    std::cout << "Given histogram with " << numBins << " bins,"
	      << " but require exactly " << numParams << std::endl;
    return ConstraintTerms;
  }

  Int_t TH1BinNumber = 0;
  for( Int_t i = 0; i < paramSet.getSize(); ++i) {

    TH1BinNumber++;

    while( uncertHist->IsBinUnderflow(TH1BinNumber) || uncertHist->IsBinOverflow(TH1BinNumber) ){
      TH1BinNumber++;
    }

    RooRealVar& gamma = (RooRealVar&) (paramSet[i]);

    std::cout << "Creating constraint for: " << gamma.GetName() 
	      << ". Type of constraint: " << type <<  std::endl;

    // Get the sigma from the hist
    // (the relative uncertainty)
    Double_t sigma = uncertHist->GetBinContent( TH1BinNumber );

    // If the sigma is <= 0, 
    // do cont create the term
    if( sigma <= 0 ){
      std::cout << "Not creating constraint term for "
		<< gamma.GetName() 
		<< " because sigma = " << sigma
		<< " (sigma<=0)" 
		<< " (TH1 bin number = " << TH1BinNumber << ")"
		<< std::endl;
      gamma.setConstant(kTRUE);
      continue;
    }
  
    // set reasonable ranges for gamma parameters
    gamma.setMax( 1 + 5*sigma );
    //    gamma.setMin( TMath::Max(1. - 5*sigma, 0.) );    
    gamma.setMin( 0. );         

    // Make Constraint Term
    std::string constrName = string(gamma.GetName()) + "_constraint";

    std::string nomName = string("nom_") + gamma.GetName();
    std::string sigmaName = string(gamma.GetName()) + "_sigma";
    std::string poisMeanName = string(gamma.GetName()) + "_poisMean";

    if( type == Constraint::Gaussian ) {

      // Type 1 : RooGaussian
    
      // Make sigma

      RooConstVar constrSigma( sigmaName.c_str(), sigmaName.c_str(), sigma );
      //proto->import( constrSigma, RecycleConflictNodes() );
      //proto->import( constrSigma );
    
      // Make "observed" value
      RooRealVar constrNom(nomName.c_str(), nomName.c_str(), 1.0,0,10);
      constrNom.setConstant( true );

      // Make the constraint: 
      RooGaussian gauss( constrName.c_str(), constrName.c_str(),
			 constrNom, gamma, constrSigma );
      
      proto->import( gauss, RecycleConflictNodes() );
      //proto->import( gauss );
      
    } else if( type == Constraint::Poisson ) {
    
      Double_t tau = 1/sigma/sigma; // this is correct Poisson equivalent to a Gaussian with mean 1 and stdev sigma

      // Make nominal "observed" value
      RooRealVar constrNom(nomName.c_str(), nomName.c_str(), tau);
      constrNom.setMin(0);
      constrNom.setConstant( true );
    
      // Make the scaling term
      std::string scalingName = string(gamma.GetName()) + "_tau";
      RooConstVar poissonScaling( scalingName.c_str(), scalingName.c_str(), tau);
    
      // Make mean for scaled Poisson
      RooProduct constrMean( poisMeanName.c_str(), poisMeanName.c_str(), RooArgSet(gamma, poissonScaling) );
      //proto->import( constrSigma, RecycleConflictNodes() );
      //proto->import( constrSigma );

      // Type 2 : RooPoisson
      RooPoisson pois(constrName.c_str(), constrName.c_str(), constrNom, constrMean);
      pois.setNoRounding(true);
      proto->import( pois, RecycleConflictNodes() );
      
    } else {

      std::cout << "Error: Did not recognize Stat Error constraint term type: "
		<< type << " for : " << paramHist.GetName() << std::endl;
    }
  
    // If the sigma value is less
    // than a supplied threshold,
    // set the variable to constant
    if( sigma < minSigma ) {
      std::cout << "Warning:  Bin " << i << " = " << sigma
		<< " and is < " << minSigma
		<< ". Setting: " << gamma.GetName() << " to constant"
		<< std::endl;
      gamma.setConstant(kTRUE);
    }
  
    constraintTermNames.push_back( constrName );    
    ConstraintTerms.add( *proto->pdf(constrName.c_str()) );

    // Add the "observed" value to the 
    // list of global observables:
    RooArgSet* globalSet = const_cast<RooArgSet*>(proto->set("globalObservables"));
  
    RooRealVar* nomVarInWorkspace = proto->var(nomName.c_str());
    if( ! globalSet->contains(*nomVarInWorkspace) ) {
      globalSet->add( *nomVarInWorkspace );	
    }
  
  } // end loop over parameters
  
  return ConstraintTerms;
  
}

} // namespace RooStats
} // namespace HistFactory

