#include <cstring>
#include "casm/app/casm_functions.hh"
#include "casm/app/DirectoryStructure.hh"
#include "casm/app/ProjectSettings.hh"
#include "casm/crystallography/Structure.hh"
#include "casm/app/AppIO.hh"
#include "casm/clusterography/Orbitree.hh"
#include "casm/clex/PrimClex.hh"
#include "casm/clusterography/jsonClust.hh"


namespace CASM {


  // ///////////////////////////////////////
  // 'clusters' function for casm
  //    (add an 'if-else' statement in casm.cpp to call this)

  int bset_command(const CommandArgs &args) {

    po::variables_map vm;

    /// Set command line options using boost program_options
    po::options_description desc("'casm bset' usage");
    desc.add_options()
    ("help,h", "Write help documentation")
    ("update,u", "Update basis set")
    ("orbits", "Pretty-print orbit prototypes")
    ("functions", "Pretty-print prototype cluster functions for each orbit")
    ("clusters", "Pretty-print all clusters")
    ("force,f", "Force overwrite");

    try {
      po::store(po::parse_command_line(args.argc, args.argv, desc), vm); // can throw
      bool call_help = false;

      /** --help option
       */
      if(vm.count("help") || call_help) {
        args.log << "\n";
        args.log << desc << std::endl;

        args.log << "DESCRIPTION" << std::endl;
        args.log << "    Generate and inspect cluster basis functions. A bspecs.json file should be available at\n"
                 << "        $ROOT/basis_set/$current_bset/bspecs.json\n"
                 << "    Run 'casm format --bspecs' for an example file.\n\n" ;

        return 0;
      }

      po::notify(vm); // throws on error, so do after help in case
      // there are any problems
    }
    catch(po::error &e) {
      args.err_log << desc << std::endl;
      args.err_log << "\nERROR: " << e.what() << std::endl << std::endl;
      return ERR_INVALID_ARG;
    }
    catch(std::exception &e) {
      args.err_log << desc << std::endl;
      args.err_log << "\nERROR: "  << e.what() << std::endl;
      return ERR_UNKNOWN;

    }

    const fs::path &root = args.root;
    if(root.empty()) {
      args.err_log.error("No casm project found");
      args.err_log << std::endl;
      return ERR_NO_PROJ;
    }

    // If 'args.primclex', use that, else construct PrimClex in 'uniq_primclex'
    // Then whichever exists, store reference in 'primclex'
    std::unique_ptr<PrimClex> uniq_primclex;
    PrimClex &primclex = make_primclex_if_not(args, uniq_primclex);
    const DirectoryStructure &dir = primclex.dir();
    const ProjectSettings &set = primclex.settings();

    if(vm.count("update")) {

      // initialize project info
      Structure prim = primclex.get_prim();

      if(!fs::is_regular_file(dir.bspecs(set.bset()))) {
        args.err_log.error("'bspecs.json' file not found");
        args.err_log << "expected basis set specifications file at: " << dir.bspecs(set.bset()) << "\n" << std::endl;
        return ERR_MISSING_INPUT_FILE;
      }


      std::vector<fs::path> filepaths({dir.clust(set.bset()),
                                       dir.basis(set.bset()),
                                       dir.clexulator_src(set.name(), set.bset()),
                                       dir.clexulator_o(set.name(), set.bset()),
                                       dir.clexulator_so(set.name(), set.bset())
                                      });

      bool any_existing_files = false;
      std::for_each(filepaths.cbegin(),
                    filepaths.cend(),
      [&](const fs::path & p) {
        if(fs::exists(p)) {
          if(!any_existing_files) {
            args.log.custom("Found existing files");
            any_existing_files = true;
          }
          args.log << "found: " << p << "\n";
        }
      });

      if(any_existing_files) {
        if(vm.count("force")) {
          args.log << "Using --force. Will overwrite existing files.\n" << std::endl;
          fs::remove(dir.clexulator_src(set.name(), set.bset()));
          fs::remove(dir.clexulator_o(set.name(), set.bset()));
          fs::remove(dir.clexulator_so(set.name(), set.bset()));
        }
        else {
          args.log << "Exiting due to existing files.  Use --force to force overwrite.\n" << std::endl;
          return ERR_EXISTING_FILE;
        }
      }

      SiteOrbitree tree(prim.lattice());

      try {
        jsonParser bspecs_json(dir.bspecs(set.bset()));

        args.log.generate("Cluster orbits");
        args.log.begin_lap();
        tree = make_orbitree(prim, bspecs_json);
        if(tree.min_num_components < 2) {
          args.err_log.error("Generating orbitree");
          args.err_log << "Custom clusters include a site with only 1 allowed component. \n";
          args.err_log << "This is not currently supported.\n" << std::endl;
          for(int nb = 0; nb < tree.size(); ++nb) {
            for(int no = 0; no < tree[nb].size(); ++no) {
              for(int ns = 0; ns < tree[nb][no].prototype.size(); ++ns) {
                if(tree[nb][no].prototype[ns].site_occupant().size() < 2) {
                  args.err_log << "--- Prototype --- " << std::endl;
                  tree[nb][no].prototype.print(args.err_log, '\n');
                  break;
                }
              }
            }
          }
          return ERR_INVALID_INPUT_FILE;
        }
        tree.generate_clust_bases();
        tree.get_index();
        args.log << "orbit generation time: " << args.log.lap_time() << " (s)\n";
        args.log << "# of cluster orbits: " << tree.Norbits << "\n";
        args.log << "# of basis functions: " << tree.basis_set_size() << "\n" << std::endl;
      }
      catch(std::exception &e) {
        args.err_log << e.what() << std::endl;
        return ERR_INVALID_INPUT_FILE;
      }

      // -- write clust.json ----------------
      args.log.write("Cluster and basis set files");
      jsonParser clust_json;
      to_json(jsonHelper(tree, prim), clust_json).write(dir.clust(set.bset()));

      args.log << "write: " << dir.clust(set.bset()) << std::endl;


      // -- write basis.json ----------------
      jsonParser basis_json;
      write_basis(tree, prim, basis_json, primclex.crystallography_tol());
      basis_json.write(dir.basis(set.bset()));

      args.log << "write: " << dir.basis(set.bset()) << std::endl;


      // -- write global Clexulator

      // get the neighbor list
      PrimNeighborList nlist(
        set.nlist_weight_matrix(),
        set.nlist_sublat_indices().begin(),
        set.nlist_sublat_indices().end()
      );

      // expand the nlist to contain 'tree'
      std::set<UnitCellCoord> nbors;
      neighborhood(std::inserter(nbors, nbors.begin()), tree, prim, primclex.crystallography_tol());
      nlist.expand(nbors.begin(), nbors.end());

      // write source code
      fs::ofstream outfile;
      outfile.open(dir.clexulator_src(set.name(), set.bset()));
      print_clexulator(prim, tree, nlist, set.clexulator(), outfile, primclex.crystallography_tol());
      outfile.close();
      args.log << "write: " << dir.clexulator_src(set.name(), set.bset()) << "\n" << std::endl;

      // compile clexulator
      primclex.clexulator(set.clex_key(), args.log);
    }
    else if(vm.count("orbits") || vm.count("clusters") || vm.count("functions")) {

      if(!fs::exists(dir.clust(set.bset()))) {
        args.err_log.error("No 'clust.json' file found");
        args.err_log << "Make sure to update your basis set with 'casm bset -u'.\n" << std::endl;
        return ERR_MISSING_DEPENDS;
      }

      primclex.orbitree(set.clex_key());

      if(vm.count("orbits")) {
        args.log.custom("Prototype clusters");
        primclex.orbitree(set.clex_key()).print_proto_clust(args.log);
        args.log << std::endl;
      }
      if(vm.count("clusters")) {
        args.log.custom("All clusters");
        primclex.orbitree(set.clex_key()).print_full_clust(args.log);
        args.log << std::endl;
      }
      if(vm.count("functions")) {
        args.log.custom("Basis functions");
        primclex.orbitree(set.clex_key()).print_proto_clust_funcs(args.log);
        args.log << std::endl;
      }
    }
    else {
      args.err_log.error("Unknown error");
      args.err_log << desc << "\n" << std::endl;
    }

    return 0;
  };

}

