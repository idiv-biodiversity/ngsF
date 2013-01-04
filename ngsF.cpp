
/*
 *
 * ngsF - NGS data individual inbreeding coefficients estimation.
 * Copyright (C) 2012  Filipe G. Vieira
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
*/

#include <sys/stat.h>
#include "shared.h"


// Cache size in Gb (to speed-up file I/O)
#define CACHE_SIZE 1

char const *version = "1.0.0";


int main (int argc, char **argv) {
	/////////////////////
	// Parse Arguments //
	/////////////////////
	params *pars = new params;
	init_pars(pars);
	parse_cmd_args(argc, argv, pars);
	if( pars->version ) {
		printf("ngsF v%s\nCompiled on %s @ %s", version, __DATE__, __TIME__);
#ifdef _USE_BGZF
		printf(" (BGZF library)\n");
#else
		printf(" (STD library)\n");
#endif

		exit(0);
	}
	if( pars->verbose >= 1 ) {
		printf("==> Input Arguments:\n");
		printf("\tglf file: %s\n\tinit_values: %s\n\tout file: %s\n\tn_ind: %d\n\tn_sites: %ld\n\tchunk_size: %ld\n\tapprox_EM: %s\n\tfast_lkl: %s\n\tmax_iters: %d\n\tmin_epsilon: %.10f\n\tn_threads: %d\n\tquick: %s\n\tversion: %s\n\tverbose: %d\n\n",
				pars->in_glf, pars->init_values, pars->out_file, pars->n_ind, pars->n_sites, pars->max_chunk_size, pars->approx_EM ? "true":"false", pars->fast_lkl ? "true":"false", pars->max_iters, pars->min_epsilon, pars->n_threads, pars->quick ? "true":"false", version, pars->verbose);
	}
	if( pars->verbose > 4 ) printf("==> Verbose values greater than 4 for debugging purpose only. Expect large amounts of info on screen\n");




	/////////////////////
	// Check Arguments //
	/////////////////////
	if(pars->in_glf == NULL)
		error("GL input file (-glf) missing!");
	else {
		pars->in_glf_type = strrchr(pars->in_glf, '.');
		if(pars->in_glf_type == NULL)
			error("invalid file type!");
	}
	if(pars->out_file == NULL)
		error("output file (-out) missing!");
	if(pars->n_ind == 0)
		error("number of individuals (-n_ind) missing!");
	if(pars->n_sites == 0)
		error("number of sites (-n_sites) missing!");
	if(pars->approx_EM)
		printf("==> Using approximated EM ML algorithm\n");



	///////////////////////
	// Check input files //
	///////////////////////
	// Get file total size
	struct stat st;
	stat(pars->in_glf, &st);
	if( pars->verbose >= 1 ) {
		if( pars->n_sites == st.st_size/sizeof(double)/pars->n_ind/3 && strcmp(pars->in_glf_type, ".glf") == 0 )
			printf("==> UNCOMP input file (\"%s\"): number of sites (%ld) match expected file size\n", pars->in_glf_type, pars->n_sites);
		else if( strcmp(pars->in_glf_type, ".glf") != 0 )
			printf("==> COMPRESSED input file (\"%s\"): number of sites (%ld) do NOT match expected file size\n", pars->in_glf_type, pars->n_sites);
		else
			error("wrong number of sites or invalid/corrupt file!");
	}


	// Adjust max_chunk_size in case of fewer sites
	if(pars->max_chunk_size > pars->n_sites) {
		if( pars->verbose >= 1 ) printf("==> Fewer sites (%ld) than chunk_size (%ld). Reducing chunk size to match number of sites\n", pars->n_sites, pars->max_chunk_size);
		pars->max_chunk_size = pars->n_sites;
	}


	// Calculate total number of chunks
	pars->n_chunks = ceil( (double) pars->n_sites/ (double) pars->max_chunk_size );
	if( pars->verbose >= 1 ) printf("==> Analysis will be run in %ld chunk(s)\n", pars->n_chunks);
	// Alocate memory for the chunk index
	pars->chunks_voffset = new int64_t[pars->n_chunks];
	memset(pars->chunks_voffset, 0, pars->n_chunks*sizeof(int64_t));

	// Adjust thread number to chunks
	if(pars->n_chunks < pars->n_threads) {
		if( pars->verbose >= 1 ) printf("==> Fewer chunks (%ld) than threads (%d). Reducing the number of threads to match number of chunks\n", pars->n_chunks, pars->n_threads);
		pars->n_threads = pars->n_chunks;
	}


	// Open input file
#ifdef _USE_BGZF
	if( pars->verbose >= 1 ) printf("==> Using BGZF I/O library\n");
	if( strcmp(pars->in_glf_type, ".bgz") != 0 )
		error("BGZF library only supports BGZIP files!");
	// Open BGZIP file
	if( (pars->in_glf_fh = bgzf_open(pars->in_glf, "rb")) < 0 )
		error("Cannot open BGZIP file!");
	bgzf_set_cache_size(pars->in_glf_fh, CACHE_SIZE * 1024uL * 1024uL * 1024uL);
#else
	if( pars->verbose >= 1 ) printf("==> Using native I/O library\n");
	if( strcmp(pars->in_glf_type, ".glf") != 0 )
		error("Standard library only supports UNCOMPRESSED GLF files!");

	// Open GLF file
	if( (pars->in_glf_fh = fopen(pars->in_glf, "rb")) == NULL )
		error("Cannot open GLF file!");

	// Allocate memory and read from the file
	pars->data = new double* [pars->n_sites];
	for(uint64_t s = 0; s < pars->n_sites; s++) {
		pars->data[s] = new double[pars->n_ind * 3];
		if( fread (pars->data[s], sizeof(double), pars->n_ind * 3, pars->in_glf_fh) != pars->n_ind * 3)
			error("cannot read GLF file!");
	}
#endif
	if( pars->in_glf_fh == NULL )
		error("cannot open GLF file!");



	///////////////////////////////////
	// Declare variables for results //
	///////////////////////////////////
	out_data *output = new out_data;
	output->site_freq = new double[pars->n_sites];
	output->site_freq_num = new double[pars->n_sites];
	output->site_freq_den = new double[pars->n_sites];
	output->site_prob_var = new double[pars->n_sites];
	output->site_tmpprob_var = new double[pars->n_sites];
	output->indF = new double[pars->n_ind];
	output->indF_num = new double[pars->n_ind];
	output->indF_den = new double[pars->n_ind];
	output->ind_lkl = new double[pars->n_ind];
	// Initialize output
	init_output(pars, output);



	//////////////////
	// Analyze Data //
	//////////////////
	if( pars->verbose >= 1 && !pars->fast_lkl && strcmp("e", pars->init_values) != 0 ) {
		printf("==> Initial LogLkl: %.15f\n", full_HWE_like(pars, output->site_freq, output->indF, 0, pars->n_ind));
		fflush(stdout);
	}
	do_EM(pars, output);
	if( pars->verbose >= 1 ) printf("\nFinal logLkl: %f\n", output->global_lkl);


	//////////////////
	// Print Output //
	//////////////////
	FILE *out_file;
	if( pars->verbose >= 1 ) printf("Printing Output...\n");

	out_file = fopen(pars->out_file, "w");
	fprintf(out_file,"Ind\tindF\tLoglkl\n");
	for(uint16_t i = 0; i < pars->n_ind; i++)
		fprintf(out_file,"%d\t%f\tNA\n",i+1, output->indF[i]);

	if( pars->verbose >= 1 ) printf("Exiting...\n");
	fclose(out_file);


	//////////////////////
	// Close Input File //
	//////////////////////
#ifdef _USE_BGZF
	bgzf_close(pars->in_glf_fh);
#else
	for(uint64_t s = 0; s < pars->n_sites; s++)
		delete [] pars->data[s];
	delete [] pars->data;
	fclose(pars->in_glf_fh);
#endif



	/////////////////
	// Free Memory //
	/////////////////
	delete [] output->site_freq;
	delete [] output->site_freq_num;
	delete [] output->site_freq_den;
	delete [] output->site_prob_var;
	delete [] output->indF;
	delete [] output->indF_num;
	delete [] output->indF_den;
	delete [] output->ind_lkl;
	delete output;
	//if( strcmp("e", pars->init_values) == 0 )
		//delete [] pars->init_values;
	delete [] pars->chunks_voffset;
	delete pars;

	return 0;
}
