#include <assert.h>


#include <mitie/named_entity_extractor.h>
#include <mitie/ner_feature_extraction.h>

#include <map>
#include <iostream>
#include <dlib/cmd_line_parser.h>

#include "conll_parser.h"

#include <dlib/svm_threaded.h>
#include <mitie/total_word_feature_extractor.h>
#include <mitie/stemmer.h>
#include <mitie/unigram_tokenizer.h>



using namespace dlib;
using namespace std;
using namespace mitie;

// ----------------------------------------------------------------------------------------

void train_chunker(const command_line_parser& parser);
void test_chunker(const command_line_parser& parser);
void train_id(const command_line_parser& parser);
void test_id(const command_line_parser& parser);
void tag_file(const command_line_parser& parser);
void tag_conll_file(const command_line_parser& parser);

// ----------------------------------------------------------------------------------------

int main(int argc, char** argv)
{
    try
    {
        command_line_parser parser;
        parser.add_option("h", "Display this help information.");
        parser.add_option("train-chunker", "train NER chunker on conll data.");
        parser.add_option("test-chunker", "test NER chunker on conll data.");
        parser.add_option("train-id", "train NER ID/classification on conll data.");
        parser.add_option("test-id", "test NER ID/classification on conll data.");
        parser.add_option("C", "Set SVM C parameter to <arg> (default 100.0).",1);
        parser.add_option("eps", "Set SVM stopping epsilon parameter to <arg> (default 0.1).",1);
        parser.add_option("threads", "Use <arg> threads when doing training (default: 4).",1);
        parser.add_option("cache-size", "Set the max cutting plane cache size to <arg> (default: 5).",1);

        parser.add_option("tag-file", "Read in a text file and tag it with the ner model in file <arg>.",1);
        parser.add_option("tag-conll-file", "Read in a CoNLL annotation file and output a copy that is tagged with the NER model from the file <arg>.",1);

        parser.parse(argc,argv);
        parser.check_option_arg_range("C", 1e-9, 1e9);
        parser.check_option_arg_range("threads", 1, 64);
        parser.check_option_arg_range("cache-size", 0, 500);

        const char* training_ops[] = {"train-chunker", "train-id"};
        const char* training_subops[] = {"C", "eps", "threads", "cache-size"};
        parser.check_sub_options(training_ops, training_subops);

        if (parser.option("h"))
        {
            cout << "Usage: ner [options]\n";
            parser.print_options(); 
            return 0;
        }

        if (parser.option("tag-file"))
        {
            tag_file(parser);
            return 0;
        }

        if (parser.option("tag-conll-file"))
        {
            tag_conll_file(parser);
            return 0;
        }

        if (parser.option("train-chunker"))
        {
            train_chunker(parser);
            return 0;
        }

        if (parser.option("test-chunker"))
        {
            test_chunker(parser);
            return 0;
        }

        if (parser.option("train-id"))
        {
            train_id(parser);
            return 0;
        }

        if (parser.option("test-id"))
        {
            test_id(parser);
            return 0;
        }

        return 0;
    }
    catch (std::exception& e)
    {
        cout << e.what() << endl;
        return 1;
    }
}

// ----------------------------------------------------------------------------------------

std::string get_mitie_models_path()
{
    const char* models = getenv("MITIE_MODELS");
    if (models==0)
        throw dlib::error("MITIE_MODELS environment variable not set.  It should contain the path to the MITIE-models repository.");
    return models;
}

void train_chunker(const command_line_parser& parser)
{
    std::vector<std::vector<std::string> > sentences;
    std::vector<std::vector<std::pair<unsigned long, unsigned long> > > chunks;
    std::vector<std::vector<unsigned long> > chunk_labels;
    parse_conll_data(parser[0], sentences, chunks, chunk_labels);
    cout << "number of sentences loaded: "<< sentences.size() << endl;

    const std::string models_path = get_mitie_models_path();

    total_word_feature_extractor fe;
    std::ifstream fin((models_path + "/total_word_feature_extractor.dat").c_str(), ios::binary);
    deserialize(fe, fin);

    cout << "words in dictionary: " << fe.get_num_words_in_dictionary() << endl;
    cout << "num features: " << fe.get_num_dimensions() << endl;


    // do the feature extraction for all the sentences
    std::vector<std::vector<matrix<float,0,1> > > samples;
    samples.reserve(sentences.size());
    for (unsigned long i = 0; i < sentences.size(); ++i)
    {
        samples.push_back(sentence_to_feats(fe, sentences[i]));
    }

    cout << "now do training" << endl;

    ner_feature_extractor nfe(fe.get_num_dimensions());
    structural_sequence_segmentation_trainer<ner_feature_extractor> trainer(nfe);

    const double C = get_option(parser, "C", 15.0);
    const double eps = get_option(parser, "eps", 0.01);
    const unsigned long num_threads = get_option(parser, "threads", 4);
    const unsigned long cache_size = get_option(parser, "cache-size", 5);
    cout << "C:           "<< C << endl;
    cout << "epsilon:     "<< eps << endl;
    cout << "num threads: "<< num_threads << endl;
    cout << "cache size:  "<< cache_size << endl;
    trainer.set_c(C);
    trainer.set_epsilon(eps);
    trainer.set_num_threads(num_threads);
    trainer.set_max_cache_size(cache_size);
    trainer.be_verbose();

    sequence_segmenter<ner_feature_extractor> segmenter = trainer.train(samples, chunks);

    cout << "num feats in chunker model: "<< segmenter.get_weights().size() << endl;
    cout << "precision, recall, f1-score: "<< test_sequence_segmenter(segmenter, samples, chunks) << endl;

    ofstream fout("trained_segmenter.dat", ios::binary);
    serialize(fe, fout);
    serialize(segmenter, fout);
}

// ----------------------------------------------------------------------------------------

void test_chunker(const command_line_parser& parser)
{
    std::vector<std::vector<std::string> > sentences;
    std::vector<std::vector<std::pair<unsigned long, unsigned long> > > chunks;
    std::vector<std::vector<unsigned long> > chunk_labels;
    parse_conll_data(parser[0], sentences, chunks, chunk_labels);
    cout << "number of sentences loaded: "<< sentences.size() << endl;

    ifstream fin("trained_segmenter.dat", ios::binary);
    total_word_feature_extractor fe;
    sequence_segmenter<ner_feature_extractor> segmenter;
    deserialize(fe, fin);
    deserialize(segmenter, fin);

    std::vector<std::vector<matrix<float,0,1> > > samples;
    samples.reserve(sentences.size());
    for (unsigned long i = 0; i < sentences.size(); ++i)
    {
        samples.push_back(sentence_to_feats(fe, sentences[i]));
    }

    cout << "precision, recall, f1-score: "<< test_sequence_segmenter(segmenter, samples, chunks) << endl;
}

// ----------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------


// ----------------------------------------------------------------------------------------

unsigned long get_label (
    const std::vector<std::pair<unsigned long, unsigned long> >& chunks,
    const std::vector<unsigned long>& chunk_labels,
    const std::pair<unsigned long, unsigned long>& range
)
/*!
    requires
        - chunks.size() == chunk_labels.size()
    ensures
        - This function checks if any of the elements of chunks are equal to range.  If so,
          then the corresponding chunk label is returned.  Otherwise a value of NOT_ENTITY
          is returned.
!*/
{
    for (unsigned long i = 0; i < chunks.size(); ++i)
    {
        if (range == chunks[i])
            return chunk_labels[i];
    }
    return NOT_ENTITY;
}

// ----------------------------------------------------------------------------------------

void train_id(const command_line_parser& parser)
{
    std::vector<std::vector<std::string> > sentences;
    std::vector<std::vector<std::pair<unsigned long, unsigned long> > > chunks;
    std::vector<std::vector<unsigned long> > chunk_labels;
    parse_conll_data(parser[0], sentences, chunks, chunk_labels);
    cout << "number of sentences loaded: "<< sentences.size() << endl;


    ifstream fin("trained_segmenter.dat", ios::binary);
    total_word_feature_extractor fe;
    sequence_segmenter<ner_feature_extractor> segmenter;
    deserialize(fe, fin);
    deserialize(segmenter, fin);

    std::vector<ner_sample_type> samples;
    std::vector<unsigned long> labels;
    for (unsigned long i = 0; i < sentences.size(); ++i)
    {
        const std::vector<matrix<float,0,1> >& sent = sentence_to_feats(fe, sentences[i]);
        std::set<std::pair<unsigned long, unsigned long> > ranges;
        // put all the true chunks into ranges
        ranges.insert(chunks[i].begin(), chunks[i].end());

        // now get all the chunks our segmenter finds
        std::vector<std::pair<unsigned long, unsigned long> > temp;
        temp = segmenter(sent);
        ranges.insert(temp.begin(), temp.end());

        // now go over all the chunks we found and label them with their appropriate NER
        // types and also do feature extraction for each.
        std::set<std::pair<unsigned long,unsigned long> >::const_iterator j;
        for (j = ranges.begin(); j != ranges.end(); ++j)
        {
            samples.push_back(extract_ner_chunk_features(sentences[i], sent, *j));
            labels.push_back(get_label(chunks[i], chunk_labels[i], *j));
        }
    }

    cout << "now do training" << endl;
    cout << "num training samples: " << samples.size() << endl;

    svm_multiclass_linear_trainer<sparse_linear_kernel<ner_sample_type>,unsigned long> trainer;

    const double C = get_option(parser, "C", 450.0);
    const double eps = get_option(parser, "eps", 0.001);
    const unsigned long num_threads = get_option(parser, "threads", 4);
    cout << "C:           "<< C << endl;
    cout << "epsilon:     "<< eps << endl;
    cout << "num_threads: "<< num_threads << endl;
    trainer.set_c(C);
    trainer.set_epsilon(eps);
    trainer.be_verbose();
    trainer.set_num_threads(num_threads);

    randomize_samples(samples, labels);
    /*
    matrix<double> res = cross_validate_multiclass_trainer(trainer, samples, labels, 5);
    cout << "5-fold cross-validation: \n" << res << endl;
    cout << "overall accuracy: "<< sum(diag(res))/sum(res) << endl;
    */

    multiclass_linear_decision_function<sparse_linear_kernel<ner_sample_type>,unsigned long> df;
    df = trainer.train(samples, labels);
    matrix<double> res = test_multiclass_decision_function(df, samples, labels);
    cout << "test on train: \n" << res << endl;
    cout << "overall accuracy: "<< sum(diag(res))/sum(res) << endl;

    cout << "C:           "<< C << endl;
    cout << "epsilon:     "<< eps << endl;
    cout << "num_threads: "<< num_threads << endl;

    std::vector<std::string> ner_labels(4);
    ner_labels[PER] = "PERSON";
    ner_labels[LOC] = "LOCATION";
    ner_labels[ORG] = "ORGANIZATION";
    ner_labels[MISC] = "MISC";
    named_entity_extractor ner(ner_labels, fe, segmenter, df);
    ofstream fout("ner_model.dat", ios::binary);
    serialize(ner, fout);
}

// ----------------------------------------------------------------------------------------

void test_id(const command_line_parser& parser)
{
    named_entity_extractor ner;
    ifstream fin("ner_model.dat", ios::binary);
    deserialize(ner, fin);

    std::vector<std::vector<std::string> > sentences;
    std::vector<std::vector<std::pair<unsigned long, unsigned long> > > chunks;
    std::vector<std::vector<unsigned long> > chunk_labels;
    parse_conll_data(parser[0], sentences, chunks, chunk_labels);
    cout << "number of sentences loaded: "<< sentences.size() << endl;


    const unsigned long num_labels = ner.get_tag_name_strings().size();
    std::vector<double> num_targets(num_labels);
    std::vector<double> num_dets(num_labels);
    std::vector<double> num_true_dets(num_labels);

    std::vector<std::pair<unsigned long, unsigned long> > ranges;
    std::vector<unsigned long> predicted_labels;

    for (unsigned long i = 0; i < sentences.size(); ++i)
    {
        ner(sentences[i], ranges, predicted_labels);

        for (unsigned long j = 0; j < ranges.size(); ++j)
        {
            const unsigned long predicted_label = predicted_labels[j];
            const unsigned long true_label = get_label(chunks[i], chunk_labels[i], ranges[j]);

            num_dets[predicted_label]++;
            if (predicted_label == true_label)
                num_true_dets[true_label]++;
        }
        for (unsigned long j = 0; j < chunk_labels[i].size(); ++j)
        {
            num_targets[chunk_labels[i][j]]++;
        }
    }

    cout << "results: "<< endl;
    for (unsigned long i = 0; i < num_targets.size(); ++i)
    {
        cout << "label: "<< i << endl;
        double prec = num_true_dets[i]/num_dets[i];
        double recall = num_true_dets[i]/num_targets[i];
        cout << "   precision: "<< prec << endl;
        cout << "   recall:    "<< recall << endl;
        cout << "   f1:        "<< 2*prec*recall/(prec+recall) << endl;
        cout << endl;
    }

    cout << "total: " << endl;
    double prec = sum(mat(num_true_dets))/sum(mat(num_dets));
    double recall = sum(mat(num_true_dets))/sum(mat(num_targets));
    cout << "   precision: "<< prec << endl;
    cout << "   recall:    "<< recall << endl;
    cout << "   f1:        "<< 2*prec*recall/(prec+recall) << endl;

}

// ----------------------------------------------------------------------------------------

extern "C"
{
    /*!
        MITIE RESOURCE MANAGEMENT POLICY
            Unless explicitly noted, you do NOT need to call free() or mitie_free() on the
            pointers returned from any MITIE API calls.  That is, if it is the caller's
            responsibility to free an object created by a MITIE API call then the
            documentation for that routine will explicitly say the caller needs to free the
            object.
    !*/

// ----------------------------------------------------------------------------------------

    typedef struct mitie_named_entity_extractor  mitie_named_entity_extractor;
    typedef struct mitie_named_entity_detections mitie_named_entity_detections;

    void mitie_free (
        void* object 
    );
    /*!
        ensures
            - Frees the resources associated with any MITIE object.
    !*/

// ----------------------------------------------------------------------------------------

    mitie_named_entity_extractor* mitie_load_named_entity_extractor (
        const char* filename
    );
    /*!
        requires
            - filename == a valid pointer to a null terminated C string
        ensures
            - The returned object MUST BE FREED by a call to mitie_free().
            - If the object can't be created then this function returns NULL
    !*/

    unsigned long mitie_get_num_possible_ner_tags (
        const mitie_named_entity_extractor* ner
    );
    /*!
        requires
            - ner != NULL
        ensures
            - A named entity extractor tags each entity with a tag.  This function returns
              the number of different tags which can be produced by the given named entity
              extractor.  Moreover, each tag is uniquely identified by a numeric ID which
              is just the index of the tag.  For example, if there are 4 possible tags then
              the numeric IDs are just 0, 1, 2, and 3.  
    !*/

    const char* mitie_get_named_entity_tagstr (
        const mitie_named_entity_extractor* ner,
        unsigned long idx
    );
    /*!
        requires
            - ner != NULL
            - idx < mitie_get_num_possible_ner_tags(ner)
        ensures
            - Each named entity tag, in addition to having a numeric ID which uniquely
              identifies it, has a text string name.  For example, if a named entity tag
              logically identifies a person then the tag string might be "PERSON". 
            - This function takes a tag ID number and returns the tag string for that tag.
            - The returned pointer is valid until mitie_free(ner) is called.
    !*/

// ----------------------------------------------------------------------------------------

    mitie_named_entity_detections* mitie_extract_entities (
        const mitie_named_entity_extractor* ner,
        const char* text
    );
    /*!
        requires
            - ner != NULL
            - text == a null terminated C string
        ensures
            - The returned object MUST BE FREED by a call to mitie_free().
            - Runs the supplied named entity extractor on text and returns a set of
              named entity detections.
            - If the object can't be created then this function returns NULL
    !*/

    unsigned long mitie_ner_get_num_detections (
        const mitie_named_entity_detections* dets
    );
    /*!
        requires
            - dets != NULL
        ensures
            - returns the number of named entity detections inside the dets object.
    !*/

    unsigned long mitie_ner_get_detection_position (
        const mitie_named_entity_detections* dets,
        unsigned long idx
    );
    /*!
        requires
            - dets != NULL
            - idx < mitie_ner_get_num_detections(dets)
        ensures
            - This function returns the position of the idx-th named entity within the
              input text.  That is, if dets was created by calling
              mitie_extract_entities(ner, TEXT) then the return value of
              mitie_ner_get_detection_position() is an index I such that TEXT[I] is the
              first character in the input text that is part of the named entity.
            - The named entity detections are stored in the order they appeared in the
              input text.  That is, for all valid IDX it is true that:
                - mitie_ner_get_detection_position(dets,IDX) < mitie_ner_get_detection_position(dets,IDX+1)
    !*/

    unsigned long mitie_ner_get_detection_length (
        const mitie_named_entity_detections* dets,
        unsigned long idx
    );
    /*!
        requires
            - dets != NULL
            - idx < mitie_ner_get_num_detections(dets)
        ensures
            - returns the length of the idx-th named entity.  That is, this function
              returns the number of chars from the input text which comprise the idx-th
              named entity detection.  
    !*/

    unsigned long mitie_ner_get_detection_tag (
        const mitie_named_entity_detections* dets,
        unsigned long idx
    );
    /*!
        requires
            - dets != NULL
            - idx < mitie_ner_get_num_detections(dets)
        ensures
            - returns a numeric value that identifies the type of the idx-th named entity.
    !*/

    const char* mitie_ner_get_detection_tagstr (
        const mitie_named_entity_detections* dets,
        unsigned long idx
    );
    /*!
        requires
            - dets != NULL
            - idx < mitie_ner_get_num_detections(dets)
        ensures
            - returns a NULL terminated C string that identifies the type of the idx-th
              named entity. 
            - The returned pointer is valid until mitie_free(dets) is called.
    !*/

// ----------------------------------------------------------------------------------------

}

extern "C"
{

// ----------------------------------------------------------------------------------------

    enum mitie_object_type
    {
        MITIE_NOT_A_MITIE_OBJECT = 0,
        MITIE_NAMED_ENTITY_EXTRACTOR = 1234,
        MITIE_NAMED_ENTITY_DETECTIONS
    };

    struct mitie_named_entity_detections_impl
    {
        std::vector<std::pair<unsigned long, unsigned long> > ranges;
        std::vector<unsigned long> predicted_labels;
        std::vector<std::string> tags;
    };

    // The reason for the impl objects is to ensure that the types in the public interface
    // are POD types.  This will let us have one single free function (mitie_free()) which
    // just looks at the headers for each object and then decides how to free them.  
    struct mitie_named_entity_extractor
    {
        mitie_object_type type;
        named_entity_extractor* impl;
    };

    struct mitie_named_entity_detections
    {
        mitie_object_type type;
        mitie_named_entity_detections_impl* impl;
    };

    void mitie_free (
        void* object 
    )
    {
        if (object == 0)
            return;

        switch(*((mitie_object_type*)object))
        {
            case MITIE_NAMED_ENTITY_EXTRACTOR:
                {
                    mitie_named_entity_extractor* obj;
                    obj = static_cast<mitie_named_entity_extractor*>(object);
                    // set this because it can help detect multiple frees
                    obj->type = MITIE_NOT_A_MITIE_OBJECT;
                    delete obj->impl;
                    delete obj;
                } break;
            case MITIE_NAMED_ENTITY_DETECTIONS:
                {
                    mitie_named_entity_detections* obj;
                    obj = static_cast<mitie_named_entity_detections*>(object);
                    // set this because it can help detect multiple frees
                    obj->type = MITIE_NOT_A_MITIE_OBJECT;
                    delete obj->impl;
                    delete obj;
                } break;
                break;
            default:
                std::cerr << "ERROR, mitie_free() called on non-MITIE object or called twice." << std::endl;
                assert(false);
                abort();
        }

    }

// ----------------------------------------------------------------------------------------

    mitie_named_entity_extractor* mitie_load_named_entity_extractor (
        const char* filename
    )
    {
        assert(filename != NULL);

        mitie_named_entity_extractor* obj = 0;
        named_entity_extractor* impl = 0;
        try
        {
            ifstream fin(filename, ios::binary);
            if (!fin)
            {
#ifndef NDEBUG
                cerr << "Error loading MITIE model file, file not found." << endl;
#endif
                return NULL;
            }

            obj = new mitie_named_entity_extractor;
            obj->type = MITIE_NAMED_ENTITY_EXTRACTOR;
            obj->impl = impl = new named_entity_extractor;
            deserialize(*impl, fin);
            return obj;
        }
        catch(std::exception& e)
        {
#ifndef NDEBUG
            cerr << "Error loading MITIE model file: " << e.what() << endl;
#endif
            delete obj;
            delete impl;
            return NULL;
        }
        catch(...)
        {
            delete obj;
            delete impl;
            return NULL;
        }
    }

    unsigned long mitie_get_num_possible_ner_tags (
        const mitie_named_entity_extractor* ner
    )
    {
        assert(ner != NULL);
        return ner->impl->get_tag_name_strings().size();
    }

    const char* mitie_get_named_entity_tagstr (
        const mitie_named_entity_extractor* ner,
        unsigned long idx
    )
    {
        assert(ner != NULL);
        assert(idx < mitie_get_num_possible_ner_tags(ner));
        return ner->impl->get_tag_name_strings()[idx].c_str();
    }

// ----------------------------------------------------------------------------------------

    mitie_named_entity_detections* mitie_extract_entities (
        const mitie_named_entity_extractor* ner,
        const char* text
    )
    {
        assert(ner != NULL);
        assert(text != NULL);

        mitie_named_entity_detections* obj = 0;
        mitie_named_entity_detections_impl* impl = 0;

        try
        {
            obj = new mitie_named_entity_detections;
            obj->type = MITIE_NAMED_ENTITY_DETECTIONS;
            obj->impl = impl = new mitie_named_entity_detections_impl;

            istringstream sin(text);
            unigram_tokenizer tok(sin);
            std::vector<std::string> words;
            std::vector<unsigned long> word_pos;
            string word;
            unsigned long pos;
            while(tok(word, pos))
            {
                words.push_back(word);
                word_pos.push_back(pos);
            }

            (*ner->impl)(words, impl->ranges, impl->predicted_labels);
            impl->tags = ner->impl->get_tag_name_strings();

            // map the ranges from token indices to character indices 
            for (unsigned long i = 0; i < impl->ranges.size(); ++i)
            {
                unsigned long begin, end;
                begin = impl->ranges[i].first;
                end = impl->ranges[i].second;

                begin = word_pos[begin];
                end = word_pos[end-1] + words[end-1].size();

                impl->ranges[i].first = begin;
                impl->ranges[i].second = end;
            }

            return obj;
        }
        catch(...)
        {
            delete obj;
            delete impl;
            return NULL;
        }

    }

    unsigned long mitie_ner_get_num_detections (
        const mitie_named_entity_detections* dets
    )
    {
        assert(dets);
        return dets->impl->ranges.size();
    }

    unsigned long mitie_ner_get_detection_position (
        const mitie_named_entity_detections* dets,
        unsigned long idx
    )
    {
        assert(dets);
        assert(idx < mitie_ner_get_num_detections(dets));
        return dets->impl->ranges[idx].first;
    }

    unsigned long mitie_ner_get_detection_length (
        const mitie_named_entity_detections* dets,
        unsigned long idx
    )
    {
        assert(dets);
        assert(idx < mitie_ner_get_num_detections(dets));
        return dets->impl->ranges[idx].second - dets->impl->ranges[idx].first;
    }

    unsigned long mitie_ner_get_detection_tag (
        const mitie_named_entity_detections* dets,
        unsigned long idx
    )
    {
        assert(dets);
        assert(idx < mitie_ner_get_num_detections(dets));
        return dets->impl->predicted_labels[idx];
    }

    const char* mitie_ner_get_detection_tagstr (
        const mitie_named_entity_detections* dets,
        unsigned long idx
    )
    {
        assert(dets);
        assert(idx < mitie_ner_get_num_detections(dets));
        unsigned long tag = dets->impl->predicted_labels[idx];
        return dets->impl->tags[tag].c_str();
    }

// ----------------------------------------------------------------------------------------

}

// ----------------------------------------------------------------------------------------

void tag_file(const command_line_parser& parser)
{

    string ner_model = parser.option("tag-file").argument();
    mitie_named_entity_extractor* ner = mitie_load_named_entity_extractor(ner_model.c_str());
    if (ner == NULL)
    {
        cout << "couldn't load model file" << endl;
        return;
    }

    ifstream fin(parser[0].c_str());
    ostringstream sout;
    sout << fin.rdbuf();
    const string text = sout.str();

    const unsigned long num_tags = mitie_get_num_possible_ner_tags(ner);
    cout << "NER tags: "<< num_tags << endl;
    for(unsigned long i = 0; i < num_tags; ++i)
        cout << "   " << mitie_get_named_entity_tagstr(ner, i) << endl;

    mitie_named_entity_detections* dets = mitie_extract_entities(ner, text.c_str());

    const unsigned long num_dets = mitie_ner_get_num_detections(dets);
    cout << "num_dets: "<< num_dets << endl;
    for (unsigned long i = 0; i < num_dets; ++i)
    {
        const unsigned long begin = mitie_ner_get_detection_position(dets, i);
        const unsigned long len = mitie_ner_get_detection_length(dets, i);
        string temp(&text[begin], &text[begin]+len);

        cout << "   " << mitie_ner_get_detection_tagstr(dets,i) << "("<< 
                         mitie_ner_get_detection_tag(dets,i) << "),  " << temp << endl;

    }


    mitie_free(dets);
    mitie_free(ner);

    /*
    string ner_model = parser.option("tag-file").argument();
    ifstream fin(ner_model.c_str(), ios::binary);
    named_entity_extractor ner;
    deserialize(ner, fin);

    fin.close();
    fin.open(parser[0].c_str());

    unigram_tokenizer tok(fin);

    std::vector<std::string> words;
    string word;
    while(tok(word))
        words.push_back(word);

    std::vector<std::pair<unsigned long, unsigned long> > ranges;
    std::vector<unsigned long> predicted_labels;
    std::vector<std::string> tags = ner.get_tag_name_strings();
    tags.push_back("O");
    ner(words, ranges, predicted_labels);

    std::vector<unsigned long> word_tags(words.size(), tags.size()-1);
    for (unsigned long i = 0; i < ranges.size(); ++i)
    {
        for (unsigned long j = ranges[i].first; j < ranges[i].second; ++j)
        {
            word_tags[j] = predicted_labels[i];
        }
    }

    for (unsigned long i = 0; i < words.size(); ++i)
    {
        cout << words[i] << "/" << tags[word_tags[i]] << " ";
    }
    */

}

// ----------------------------------------------------------------------------------------

void tag_conll_file(const command_line_parser& parser)
{
    string ner_model = parser.option("tag-conll-file").argument();
    ifstream fin(ner_model.c_str(), ios::binary);
    named_entity_extractor ner;
    deserialize(ner, fin);


    std::vector<labeled_sentence> conll_data = parse_conll_data (parser[0]);
    std::vector<std::vector<std::string> > tokens;
    std::vector<std::vector<BIO_label> > labels;
    separate_labels_from_tokens(conll_data, tokens, labels);

    std::vector<std::pair<unsigned long, unsigned long> > ranges;
    std::vector<unsigned long> predicted_labels;
    for (unsigned long i = 0; i < tokens.size(); ++i)
    {
        ner(tokens[i], ranges, predicted_labels);
        labels[i].assign(labels[i].size(),O);
        for (unsigned long j = 0; j < ranges.size(); ++j)
        {
            for (unsigned long k = ranges[j].first; k < ranges[j].second; ++k)
            {
                if (j > 0 && ranges[j].first == ranges[j-1].second && predicted_labels[j] == predicted_labels[j-1])
                {
                    if (predicted_labels[j] == PER)
                        labels[i][k] = B_PER; 
                    else if (predicted_labels[j] == ORG)
                        labels[i][k] = B_ORG; 
                    else if (predicted_labels[j] == LOC)
                        labels[i][k] = B_LOC; 
                    else if (predicted_labels[j] == MISC)
                        labels[i][k] = B_MISC; 
                }
                else
                {
                    if (predicted_labels[j] == PER)
                        labels[i][k] = I_PER; 
                    else if (predicted_labels[j] == ORG)
                        labels[i][k] = I_ORG; 
                    else if (predicted_labels[j] == LOC)
                        labels[i][k] = I_LOC; 
                    else if (predicted_labels[j] == MISC)
                        labels[i][k] = I_MISC; 
                }
            }
        }
    }

    print_conll_data(conll_data, labels);
}

// ----------------------------------------------------------------------------------------
