#include <boost/program_options.hpp>
#include <iostream>
#include <string>

#include "logging/logging.h"
#include "repo.h"

namespace po = boost::program_options;

int main(int argc, char **argv) {
  po::options_description desc("aktualizr_repo command line options");
  // clang-format off
  desc.add_options()
    ("help,h", "print usage")
    ("command", po::value<std::string>(), "generate|image|target")
    ("path", po::value<boost::filesystem::path>(), "path to the repository")
    ("filename", po::value<std::string>(), "path to the image")
    ("expires", po::value<std::string>(), "expiration time");
  // clang-format on

  po::positional_options_description positionalOptions;
  positionalOptions.add("command", 1);
  positionalOptions.add("path", 1);
  positionalOptions.add("filename", 1);

  try {
    logger_init();
    logger_set_threshold(boost::log::trivial::info);
    po::variables_map vm;
    po::basic_parsed_options<char> parsed_options =
        po::command_line_parser(argc, argv).options(desc).positional(positionalOptions).run();
    po::store(parsed_options, vm);
    po::notify(vm);
    if (vm.count("help") != 0) {
      std::cout << desc << std::endl;
      exit(EXIT_SUCCESS);
    }

    if (vm.count("command") != 0 && vm.count("path") != 0) {
      std::string expiration_time;
      if (vm.count("expires") != 0) {
        expiration_time = vm["expires"].as<std::string>();
      }
      Repo repo(vm["path"].as<boost::filesystem::path>(), expiration_time);
      std::string command = vm["command"].as<std::string>();
      if (command == "generate") {
        repo.generateRepo();
      } else if (command == "image") {
        repo.addImage(vm["filename"].as<std::string>());
      } else if (command == "target") {
        repo.copyTarget(vm["filename"].as<std::string>());
      } else {
        std::cout << desc << std::endl;
        exit(EXIT_FAILURE);
      }
    } else {
      std::cout << desc << std::endl;
      exit(EXIT_FAILURE);
    }

    exit(EXIT_SUCCESS);

  } catch (const po::error &o) {
    std::cout << o.what() << std::endl;
    std::cout << desc;
    return EXIT_FAILURE;
  }
}
// vim: set tabstop=2 shiftwidth=2 expandtab:
