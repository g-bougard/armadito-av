# Modify version here
OS_V=ubuntu-14.04-64
DIR=$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )

PACKAGE=$1

# Modify git repositories' paths here
CORE_SRC=$HOME/armadito-av/core
CLAMAV_SRC=$HOME/armadito-mod-clamav
H1_SRC=$HOME/armadito-mod-h1
PDF_SRC=$HOME/armadito-mod-pdf
GUI_SRC=$HOME/armadito-gui

set -e

if [[ $PACKAGE == "core" || $PACKAGE == "" ]];
then
	./configure.sh -i $CORE_SRC -p core -o $OS_V
	./compile.sh -p core -o $OS_V
fi

if [[ $PACKAGE == "clamav" || $PACKAGE == "" ]];
then
	./configure.sh -i $CLAMAV_SRC -p clamav -o $OS_V
	./compile.sh -p clamav -o $OS_V
fi

if [[ $PACKAGE == "moduleH1" || $PACKAGE == "" ]];
then
	./configure.sh -i $H1_SRC -p moduleH1 -o $OS_V
	./compile.sh -p moduleH1 -o $OS_V
fi

if [[ $PACKAGE == "modulePDF" || $PACKAGE == "" ]];
then
	./configure.sh -i $PDF_SRC -p modulePDF -o $OS_V
	./compile.sh -p modulePDF -o $OS_V
fi

#if [[ $PACKAGE == "gui" || $PACKAGE == "" ]];
#then
#	./configure.sh -p gui -o $OS_V
#	./compile.sh -p gui -o $OS_V
#fi
