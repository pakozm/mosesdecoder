/***********************************************************************
Moses - factored phrase-based language decoder
Copyright (C) 2009 University of Edinburgh

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
***********************************************************************/

#include "Decoder.h"
#include "Manager.h"
#include "Sentence.h"
#include "InputType.h"
#include "TranslationSystem.h"
#include "Phrase.h"
#include "TrellisPathList.h"
#include "DummyScoreProducers.h"

using namespace std;
using namespace Moses;


namespace Mira {

  //Decoder::~Decoder() {}

  /**
    * Allocates a char* and copies string into it.
  **/
  static char* strToChar(const string& s) {
    char* c = new char[s.size()+1];
    strcpy(c,s.c_str());
    return c;
  }
      
  void initMoses(const string& inifile, int debuglevel,  int argc, char** argv) {
    static int BASE_ARGC = 5;
    Parameter* params = new Parameter();
    char ** mosesargv = new char*[BASE_ARGC + argc];
    mosesargv[0] = strToChar("-f");
    mosesargv[1] = strToChar(inifile);
    mosesargv[2] = strToChar("-v");
    stringstream dbgin;
    dbgin << debuglevel;
    mosesargv[3] = strToChar(dbgin.str());
    mosesargv[4] = strToChar("-mbr"); //so we can do nbest
    
    for (int i = 0; i < argc; ++i) {
      mosesargv[BASE_ARGC + i] = argv[i];
    }
    params->LoadParam(BASE_ARGC + argc,mosesargv);
    StaticData::LoadDataStatic(params);
    for (int i = 0; i < BASE_ARGC; ++i) {
      delete[] mosesargv[i];
    }
    delete[] mosesargv;
  }
 
  MosesDecoder::MosesDecoder(const vector<vector<string> >& refs, bool useScaledReference, bool scaleByInputLength)
		: m_manager(NULL) {
	  // force initialisation of the phrase dictionary
      const StaticData &staticData = StaticData::Instance();
      const TranslationSystem& system = staticData.GetTranslationSystem(TranslationSystem::DEFAULT);

      // Add the bleu feature
      m_bleuScoreFeature = new BleuScoreFeature(useScaledReference, scaleByInputLength);
      (const_cast<TranslationSystem&>(system)).AddFeatureFunction(m_bleuScoreFeature);
      m_bleuScoreFeature->LoadReferences(refs);
  }
  
  void MosesDecoder::cleanup() {
	  delete m_manager;
	  delete m_sentence;
  }

  vector<const Word*> MosesDecoder::getNBest(const std::string& source,
                              size_t sentenceid,
                              size_t count,
                              float bleuObjectiveWeight, 
                              float bleuScoreWeight,
                              vector< ScoreComponentCollection>& featureValues,
                              vector< float>& bleuScores,
                              bool oracle)
  {
	StaticData &staticData = StaticData::InstanceNonConst();

	m_sentence = new Sentence(Input);
    stringstream in(source + "\n");
    const std::vector<FactorType> &inputFactorOrder = staticData.GetInputFactorOrder();
    m_sentence->Read(in,inputFactorOrder);
    const TranslationSystem& system = staticData.GetTranslationSystem
        (TranslationSystem::DEFAULT);

    //set the weight for the bleu feature
    ostringstream bleuWeightStr;
    bleuWeightStr << bleuObjectiveWeight;
    PARAM_VEC bleuWeight(1,bleuWeightStr.str());
    staticData.GetParameter()->OverwriteParam("weight-bl", bleuWeight);
    staticData.ReLoadParameter();

    m_bleuScoreFeature->SetCurrentSourceLength((*m_sentence).GetSize());
    m_bleuScoreFeature->SetCurrentReference(sentenceid);

    //run the decoder
    m_manager = new Moses::Manager(*m_sentence, staticData.GetSearchAlgorithm(), &system); 
    m_manager->ProcessSentence();
    TrellisPathList sentences;
    m_manager->CalcNBest(count,sentences);
						
    // read off the feature values and bleu scores for each sentence in the nbest list
    Moses::TrellisPathList::const_iterator iter;
    for (iter = sentences.begin() ; iter != sentences.end() ; ++iter) {
    	const Moses::TrellisPath &path = **iter;
    	featureValues.push_back(path.GetScoreBreakdown());
    	float bleuScore = getBleuScore(featureValues.back());
    	bleuScores.push_back(bleuScore);

    	//std::cout << "Score breakdown: " << path.GetScoreBreakdown() << endl;
    	float scoreWithoutBleu = path.GetTotalScore() - bleuObjectiveWeight * bleuScore;
    	cerr << "Total score: " << path.GetTotalScore() << ", Score w/o bleu: " << scoreWithoutBleu << ", Bleu: " << bleuScore << endl;

    	// set bleu score to zero in the feature vector since we do not want to optimise its weight
    	setBleuScore(featureValues.back(), 0);

    	/*// construct translation string
    	vector<const Word*> sentence;
        Phrase targetPhrase = path.GetTargetPhrase();

        for (size_t pos = 0; pos < targetPhrase.GetSize(); ++pos) {
        	const Word &word = targetPhrase.GetWord(pos);
        	Word *newWord = new Word(word);
        	cerr << *newWord << " ";
    	}

        cerr << endl;*/
    }

    // get the best
    vector<const Word*> best;
    if (oracle) {

        assert(sentences.GetSize() > 0);
        const TrellisPath &path = sentences.at(0);
        Phrase bestPhrase = path.GetTargetPhrase();

        for (size_t pos = 0; pos < bestPhrase.GetSize(); ++pos) {
        	const Word &word = bestPhrase.GetWord(pos);
        	Word *newWord = new Word(word);
        	best.push_back(newWord);
    	}
    }

    return best;
  }


  float MosesDecoder::getBleuScore(const ScoreComponentCollection& scores) {
    return scores.GetScoreForProducer(m_bleuScoreFeature);
  }

  void MosesDecoder::setBleuScore(ScoreComponentCollection& scores, float bleu) {
    scores.Assign(m_bleuScoreFeature, bleu);
  }

  ScoreComponentCollection MosesDecoder::getWeights() {
    return StaticData::Instance().GetAllWeights();
  }

  void MosesDecoder::setWeights(const ScoreComponentCollection& weights) {
    cerr << "New weights: " << weights << endl;
    StaticData::InstanceNonConst().SetAllWeights(weights);
  }

  void MosesDecoder::updateHistory(const vector<const Word*>& words) {
    m_bleuScoreFeature->UpdateHistory(words);
  }
	
} 

