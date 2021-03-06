/***************************************************************************
                              simmessage.cpp
                             ----------------
    begin                : Sat Sep 6 2003
    copyright            : (C) 2003 by Michael Margraf
    email                : michael.margraf@alumni.tu-berlin.de
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include <stdlib.h>

#include <qlabel.h>
#include <qlayout.h>
#include <qvgroupbox.h>
#include <qhgroupbox.h>
#include <qhbox.h>
#include <qtimer.h>
#include <qpushbutton.h>
#include <qprogressbar.h>
#include <qtextedit.h>
#include <qdatetime.h>
#include <qregexp.h>

#include "simmessage.h"
#include "main.h"
#include "qucs.h"
#include "textdoc.h"
#include "schematic.h"
#include "components/opt_sim.h"


SimMessage::SimMessage(QWidget *w, QWidget *parent)
		: QDialog(parent, 0, FALSE, Qt::WDestructiveClose)
{
  setCaption(tr("Qucs Simulation Messages"));
  QucsDoc *Doc;
  DocWidget = w;
  if(DocWidget->inherits("QTextEdit"))
    Doc = (QucsDoc*) ((TextDoc*)DocWidget);
  else
    Doc = (QucsDoc*) ((Schematic*)DocWidget);

  DocName = Doc->DocName;
  DataDisplay = Doc->DataDisplay;
  QFileInfo Info(DocName);
  DataSet = QDir::convertSeparators(Info.dirPath()) +
    QDir::separator() + Doc->DataSet;
  showBias = Doc->showBias;     // save some settings as the document...
  SimOpenDpl = Doc->SimOpenDpl; // ...could be closed during the simulation.

  all = new QVBoxLayout(this);
  all->setSpacing(5);
  all->setMargin(5);
  QVGroupBox *Group1 = new QVGroupBox(tr("Progress:"),this);
  all->addWidget(Group1);

  ProgText = new QTextEdit(Group1);
  ProgText->setTextFormat(Qt::PlainText);
  ProgText->setReadOnly(true);
  ProgText->setWordWrap(QTextEdit::NoWrap);
  ProgText->setMinimumSize(400,80);
  wasLF = false;

  QHGroupBox *HGroup = new QHGroupBox(this);
  HGroup->setInsideMargin(5);
  HGroup->setInsideSpacing(5);
  all->addWidget(HGroup);
  new QLabel(tr("Progress:"), HGroup);
  SimProgress = new QProgressBar(HGroup);
//  SimProgress->setPercentageVisible(false);

  QVGroupBox *Group2 = new QVGroupBox(tr("Errors and Warnings:"),this);
  all->addWidget(Group2);

  ErrText = new QTextEdit(Group2);
  ErrText->setTextFormat(Qt::PlainText);
  ErrText->setReadOnly(true);
  ErrText->setWordWrap(QTextEdit::NoWrap);
  ErrText->setMinimumSize(400,80);

  QHBox *Butts = new QHBox(this);
  all->addWidget(Butts);

  Display = new QPushButton(tr("Goto display page"), Butts);
  Display->setDisabled(true);
  connect(Display,SIGNAL(clicked()),SLOT(slotDisplayButton()));

  Abort = new QPushButton(tr("Abort simulation"), Butts);
  connect(Abort,SIGNAL(clicked()),SLOT(reject()));
}

SimMessage::~SimMessage()
{
  if(SimProcess.isRunning())  SimProcess.kill();
  delete all;
}

// ------------------------------------------------------------------------
bool SimMessage::startProcess()
{
  Abort->setText(tr("Abort simulation"));
  Display->setDisabled(true);

  ProgText->insert(tr("Starting new simulation on ")+
                   QDate::currentDate().toString("ddd dd. MMM yyyy"));
  ProgText->insert(tr(" at ")+
                   QTime::currentTime().toString("hh:mm:ss")+"\n\n");

  SimProcess.blockSignals(false);
  if(SimProcess.isRunning()) {
    ErrText->insert(tr("ERROR: Simulator is still running!"));
    FinishSimulation(-1);
    return false;
  }

  Collect.clear();  // clear list for NodeSets, SPICE components etc.
  ProgText->insert(tr("creating netlist... "));
  NetlistFile.setName(QucsHomeDir.filePath("netlist.txt"));
   if(!NetlistFile.open(IO_WriteOnly)) {
    ErrText->insert(tr("ERROR: Cannot write netlist file!"));
    FinishSimulation(-1);
    return false;
  }

  Stream.setDevice(&NetlistFile);

  if(!DocWidget->inherits("QTextEdit")) {
    SimPorts =
       ((Schematic*)DocWidget)->prepareNetlist(Stream, Collect, ErrText);
    if(SimPorts < -5) {
      NetlistFile.close();
      FinishSimulation(-1);
      return false;
    }
  }
  Collect.append("*");   // mark the end


  disconnect(&SimProcess, 0, 0, 0);
  connect(&SimProcess, SIGNAL(readyReadStderr()), SLOT(slotDisplayErr()));
  connect(&SimProcess, SIGNAL(readyReadStdout()),
                       SLOT(slotReadSpiceNetlist()));
  connect(&SimProcess, SIGNAL(processExited()),
                       SLOT(slotFinishSpiceNetlist()));

  nextSPICE();
  return true;
  // Since now, the Doc pointer may be obsolete, as the user could have
  // closed the schematic !!!
}
  
// ---------------------------------------------------
// Converts a spice netlist into Qucs format and outputs it.
void SimMessage::nextSPICE()
{
  QString Line;
  for(;;) {  // search for next SPICE component
    Line = *(Collect.begin());
    Collect.remove(Collect.begin());
    if(Line == "*") {  // worked on all components ?
      startSimulator(); // <<<<<================== go on ===
      return;
    }
    if(Line.left(5) == "SPICE") {
      if(Line.at(5) != 'o') insertSim = true;
      else insertSim = false;
      break;
    }
    Collect.append(Line);
  }


  QString FileName = Line.section('"', 1,1);
  Line = Line.section('"', 2);  // port nodes
  if(Line.isEmpty())  makeSubcircuit = false;
  else  makeSubcircuit = true;

  QStringList com;
  com << (QucsSettings.BinDir + "qucsconv");
  if(makeSubcircuit)
    com << "-g" << "_ref";
  com << "-if" << "spice" << "-of" << "qucs";
  SimProcess.setArguments(com);

  QFile SpiceFile;
  if(FileName.find(QDir::separator()) < 0)  // add path ?
    SpiceFile.setName(QucsWorkDir.path() + QDir::separator() + FileName);
  else
    SpiceFile.setName(FileName);
  if(!SpiceFile.open(IO_ReadOnly)) {
    ErrText->insert(tr("ERROR: Cannot open SPICE file \"%1\".").arg(FileName));
    FinishSimulation(-1);
    return;
  }


  if(makeSubcircuit) {
    Stream << "\n.Def:" << properName(FileName) << " ";
  
    Line.replace(',', ' ');
    Stream << Line;
    if(!Line.isEmpty()) Stream << " _ref";
  }
  Stream << "\n";


  ProgressText = "";
  if(!SimProcess.start()) {
    ErrText->insert(tr("ERROR: Cannot start QucsConv!"));
    FinishSimulation(-1);
    return;
  }

  QByteArray SpiceContent = SpiceFile.readAll();
  SpiceFile.close();
  SimProcess.writeToStdin(SpiceContent);
  connect(&SimProcess, SIGNAL(wroteToStdin()), SLOT(slotCloseStdin()));
}

// ------------------------------------------------------------------------
void SimMessage::slotCloseStdin()
{
  SimProcess.closeStdin();
  disconnect(&SimProcess, SIGNAL(wroteToStdin()), 0, 0);
}

// ------------------------------------------------------------------------
void SimMessage::slotReadSpiceNetlist()
{
  int i;
  QString s;
  ProgressText += QString(SimProcess.readStdout());

  while((i = ProgressText.find('\n')) >= 0) {

    s = ProgressText.left(i);
    ProgressText.remove(0, i+1);


    s = s.stripWhiteSpace();
    if(s.isEmpty()) continue;
    if(s.at(0) == '#') continue;
    if(s.at(0) == '.') if(s.left(5) != ".Def:") { // insert simulations later
      if(insertSim) Collect.append(s);
      continue;
    }
    Stream << "  " << s << '\n';
  }
}


// ------------------------------------------------------------------------
void SimMessage::slotFinishSpiceNetlist()
{
  if(makeSubcircuit)
    Stream << ".Def:End\n\n";

  nextSPICE();
}

// ------------------------------------------------------------------------
#ifdef __MINGW32__
#include <windows.h>
static QString getShortPathName(QString longpath) {
  const char * lpath = QDir::convertSeparators(longpath).ascii();
  char spath[2048];
  int len = GetShortPathNameA(lpath,spath,sizeof(spath)-1);
  spath[len] = '\0';
  return QString(spath);
}
#endif

// ------------------------------------------------------------------------
void SimMessage::startSimulator()
{
  // Using the Doc pointer here is risky as the user may have closed
  // the schematic, but converting the SPICE netlists is (hopefully)
  // faster than the user (I have no other idea).

  QString SimTime;
  QStringList CommandLine;
  QString SimPath = QDir::convertSeparators (QucsHomeDir.absPath());
#ifdef __MINGW32__
  QString QucsDigi = "qucsdigi.bat";
#else
  QString QucsDigi = "qucsdigi";
#endif
  SimOpt = NULL;

  if(DocWidget->inherits("QTextEdit")) {
    // Take VHDL file in memory as it could contain unsaved changes.
    Stream << ((TextDoc*)DocWidget)->text();
    NetlistFile.close();
    ProgText->insert(tr("done.\n"));  // of "creating netlist... 

    SimTime = ((TextDoc*)DocWidget)->SimTime;
#ifdef __MINGW32__
    CommandLine << getShortPathName(QucsSettings.BinDir + QucsDigi)
		<< "netlist.txt" << DataSet
		<< SimTime << getShortPathName(SimPath)
		<< getShortPathName(QucsSettings.BinDir);
#else
    CommandLine << QucsSettings.BinDir + QucsDigi << "netlist.txt" << DataSet
       << SimTime << SimPath << QucsSettings.BinDir;
#endif
  }
  else {
    // output NodeSets, SPICE simulations etc.
    Stream << Collect.join("\n") << '\n';
    SimTime = ((Schematic*)DocWidget)->createNetlist(Stream, SimPorts);
    NetlistFile.close();
    if(SimTime.at(0) == '�') {
      ErrText->insert(SimTime.mid(1));
      FinishSimulation(-1);
      return;
    }
    ProgText->insert(tr("done.\n"));  // of "creating netlist... 

    if(SimPorts < 0) {
      if((SimOpt = findOptimization((Schematic*)DocWidget))) {
	((Optimize_Sim*)SimOpt)->createASCOnetlist();
	CommandLine << QucsSettings.AscoDir + "asco" << "-qucs" <<
	  QucsHomeDir.filePath("asco_netlist.txt") << "-o" << "asco_out";
      }
      else {
	CommandLine << QucsSettings.BinDir + "qucsator" << "-b" << "-g"
           << "-i" << QucsHomeDir.filePath("netlist.txt") << "-o" << DataSet;
      }
    } else {
#ifdef __MINGW32__
      CommandLine << getShortPathName(QucsSettings.BinDir + QucsDigi)
		  << "netlist.txt" << DataSet
		  << SimTime << getShortPathName(SimPath)
		  << getShortPathName(QucsSettings.BinDir) << "-c";
#else
      CommandLine << QucsSettings.BinDir + QucsDigi << "netlist.txt"
         << DataSet << SimTime << SimPath << QucsSettings.BinDir << "-c";
#endif
    }
  }

  SimProcess.setArguments(CommandLine);

  disconnect(&SimProcess, 0, 0, 0);
  connect(&SimProcess, SIGNAL(readyReadStderr()), SLOT(slotDisplayErr()));
  connect(&SimProcess, SIGNAL(readyReadStdout()), SLOT(slotDisplayMsg()));
  connect(&SimProcess, SIGNAL(processExited()), SLOT(slotSimEnded()));

#ifdef SPEEDUP_PROGRESSBAR
  waitForUpdate = false;
#endif
  wasLF = false;
  ProgressText = "";
  if(!SimProcess.start()) {
    ErrText->insert(tr("ERROR: Cannot start simulator!"));
    FinishSimulation(-1);
    return;
  }
}

// ------------------------------------------------------------------------
Component * SimMessage::findOptimization(Schematic *Doc) {
  Component *pc;
  for(pc=Doc->Components->first(); pc!=0; pc=Doc->Components->next())
    if(pc->isActive)
      if(pc->Model == ".Opt")
	return pc;
  return NULL;
}


// ------------------------------------------------------------------------
// Is called when the process sends an output to stdout.
void SimMessage::slotDisplayMsg()
{
  int i;
  ProgressText += QString(SimProcess.readStdout());
  if(wasLF) {
    i = ProgressText.findRev('\r');
#ifdef __MINGW32__
    while (i > 1 && ProgressText.at(i+1) == '\n') {
      i = ProgressText.findRev('\r', i-1);
    }
#endif /* __MINGW32__ */
    if(i > 1) {
#ifdef SPEEDUP_PROGRESSBAR
      iProgress = 10*int(ProgressText.at(i-2).latin1()-'0') +
                     int(ProgressText.at(i-1).latin1()-'0');
      if(!waitForUpdate) {
        QTimer::singleShot(20, this, SLOT(slotUpdateProgressBar()));
        waitForUpdate = true;
      }
#else
      SimProgress->setProgress(
         10*int(ProgressText.at(i-2).latin1()-'0') +
            int(ProgressText.at(i-1).latin1()-'0'), 100);
#endif
      ProgressText.remove(0, i+1);
    }

    if(ProgressText.at(0).latin1() <= '\t')
      return;
  }
  else {
    i = ProgressText.find('\t');
    if(i >= 0) {
      wasLF = true;
      ProgText->insert(ProgressText.left(i));
      ProgressText.remove(0, i+1);
      return;
    }
  }

  ProgText->insert(ProgressText);
  ProgressText = "";
  wasLF = false;
}

#ifdef SPEEDUP_PROGRESSBAR
// ------------------------------------------------------------------------
void SimMessage::slotUpdateProgressBar()
{
  SimProgress->setProgress(iProgress, 100);
  waitForUpdate = false;
}
#endif

// ------------------------------------------------------------------------
// Is called when the process sends an output to stderr.
void SimMessage::slotDisplayErr()
{
  int par = ErrText->paragraphs();
  int idx = ErrText->paragraphLength(par-1);
  ErrText->setCursorPosition(par-1,idx);
  ErrText->insert(QString(SimProcess.readStderr()));
}

// ------------------------------------------------------------------------
// Is called when the simulation process terminates.
void SimMessage::slotSimEnded()
{
  int stat = (!SimProcess.normalExit()) ? -1 : SimProcess.exitStatus();
  FinishSimulation(stat);
}

// ------------------------------------------------------------------------
// Is called when the simulation ended with errors before starting simulator
// process.
void SimMessage::FinishSimulation(int Status)
{
  Abort->setText(tr("Close window"));
  Display->setDisabled(false);
  SimProgress->setProgress(100, 100);   // progress bar to 100%

  QDate d = QDate::currentDate();   // get date of today
  QTime t = QTime::currentTime();   // get time

  if(Status == 0) {
    ProgText->insert(tr("\nSimulation ended on ")+
                     d.toString("ddd dd. MMM yyyy"));
    ProgText->insert(tr(" at ")+t.toString("hh:mm:ss")+"\n");
    ProgText->insert(tr("Ready.\n"));
  }
  else {
    ProgText->insert(tr("\nErrors occured during simulation on ")+
                     d.toString("ddd dd. MMM yyyy"));
    ProgText->insert(tr(" at ")+t.toString("hh:mm:ss")+"\n");
    ProgText->insert(tr("Aborted.\n"));
  }

  QFile file(QucsHomeDir.filePath("log.txt"));  // save simulator messages
  if(file.open(IO_WriteOnly)) {
    int z;
    QTextStream stream(&file);
    stream << tr("Output:\n----------\n\n");
    for(z=0; z<=ProgText->paragraphs(); z++)
      stream << ProgText->text(z) << "\n";
    stream << tr("\n\n\nErrors:\n--------\n\n");
    for(z=0; z<ErrText->paragraphs(); z++)
      stream << ErrText->text(z) << "\n";
    file.close();
  }

  if(Status == 0) {
    if(SimOpt) { // save optimization data
      QFile ifile(QucsHomeDir.filePath("asco_out.dat"));
      QFile ofile(DataSet);
      if(ifile.open(IO_ReadOnly)) {
	if(ofile.open(IO_WriteOnly)) {
	  QByteArray data = ifile.readAll();
	  ofile.writeBlock(data);
	  ofile.close();
	}
	ifile.close();
      }
      if(((Optimize_Sim*)SimOpt)->loadASCOout())
	((Schematic*)DocWidget)->setChanged(true,true);
    }
  }

  emit SimulationEnded(Status, this);
}

// ------------------------------------------------------------------------
// To call accept(), which is protected, from the outside.
void SimMessage::slotClose()
{
  accept();
}

// ------------------------------------------------------------------------
void SimMessage::slotDisplayButton()
{
  emit displayDataPage(DocName, DataDisplay);
  accept();
}
