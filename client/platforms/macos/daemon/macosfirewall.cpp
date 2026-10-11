// Copyright (c) 2023 Private Internet Access, Inc.
//
// This file is part of the Private Internet Access Desktop Client.
//
// The Private Internet Access Desktop Client is free software: you can
// redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, either version 3 of
// the License, or (at your option) any later version.
//
// The Private Internet Access Desktop Client is distributed in the hope that
// it will be useful, but WITHOUT ANY WARRANTY; without even the implied
// warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with the Private Internet Access Desktop Client.  If not, see
// <https://www.gnu.org/licenses/>.

// Copyright (c) 2024 AmneziaVPN
// This file has been modified for AmneziaVPN
//
// This file is based on the work of the Private Internet Access Desktop Client.
// The original code of the Private Internet Access Desktop Client is copyrighted (c) 2023 Private Internet Access, Inc. and licensed under GPL3.
//
// The modified version of this file is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this file. If not, see <https://www.gnu.org/licenses/>.

#include "macosfirewall.h"

#include "logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

#include <time.h>

#ifndef AMNEZIA_INSTANCE_APP_NAME
#  define AMNEZIA_INSTANCE_APP_NAME "AmneziaVPN"
#endif
#ifndef AMNEZIA_INSTANCE_SUFFIX
#  define AMNEZIA_INSTANCE_SUFFIX ""
#endif

#define ResourceDir (qApp->applicationDirPath() + "/pf")
#define DaemonDataDir \
  QStringLiteral("/Library/Application Support/" AMNEZIA_INSTANCE_APP_NAME "/pf")

namespace {
Logger logger("MacOSFirewall");

const QString kResourceAnchor = QStringLiteral("amn");
const QString kRootAnchor = QStringLiteral("amn" AMNEZIA_INSTANCE_SUFFIX);
const bool kIsSideBySide = !QStringLiteral(AMNEZIA_INSTANCE_SUFFIX).isEmpty();
const QByteArray kPfWarning =
    "pfctl: Use of -f option, could result in flushing of rules\n"
    "present in the main ruleset added by the system at startup.\n"
    "See /etc/pf.conf for further details.\n";

struct ProcessResult {
  int exitCode = -2;
  QByteArray standardOutput;
  QByteArray standardError;
};

ProcessResult runProcess(const QString& program, const QStringList& arguments,
                         const QByteArray& input = {},
                         bool ignoreErrors = false, bool logOutput = true) {
  QProcess process;
  process.start(program, arguments, QProcess::ReadWrite);
  if (!input.isEmpty()) {
    process.write(input);
  }
  process.closeWriteChannel();

  ProcessResult result;
  if (!process.waitForFinished() ||
      process.error() == QProcess::FailedToStart) {
    result.exitCode = -2;
  } else if (process.exitStatus() != QProcess::NormalExit) {
    result.exitCode = -1;
  } else {
    result.exitCode = process.exitCode();
  }
  result.standardOutput = process.readAllStandardOutput().trimmed();
  result.standardError =
      process.readAllStandardError().replace(kPfWarning, "").trimmed();
  if ((result.exitCode != 0 || !result.standardError.isEmpty()) &&
      !ignoreErrors) {
    logger.info() << "(" << result.exitCode << ")" << program << arguments;
  }
  if (logOutput && !result.standardOutput.isEmpty()) {
    logger.info() << result.standardOutput;
  }
  if (logOutput && !result.standardError.isEmpty()) {
    logger.info() << result.standardError;
  }
  return result;
}

ProcessResult runPfctl(const QStringList& arguments,
                       const QByteArray& input = {},
                       bool ignoreErrors = false) {
  return runProcess(QStringLiteral("/sbin/pfctl"), arguments, input,
                    ignoreErrors);
}

bool isOwnRootDeclaration(const QString& line, const QString& keyword) {
  const QString pattern =
      QStringLiteral("^%1\\s+\"%2(?:/\\*)?\"(?:\\s|$)")
          .arg(QRegularExpression::escape(keyword),
               QRegularExpression::escape(kRootAnchor));
  return QRegularExpression(pattern).match(line.trimmed()).hasMatch();
}

QStringList withoutOwnRootDeclarations(const QByteArray& rules,
                                       const QStringList& keywords) {
  QStringList filtered;
  for (const QString& line :
       QString::fromUtf8(rules).split('\n', Qt::SkipEmptyParts)) {
    bool ownDeclaration = false;
    for (const QString& keyword : keywords) {
      if (isOwnRootDeclaration(line, keyword)) {
        ownDeclaration = true;
        break;
      }
    }
    if (!ownDeclaration) {
      filtered.append(line);
    }
  }
  return filtered;
}

bool replaceRootRules(const QString& showOption, const QString& loadOption,
                      const QStringList& keywords,
                      const QStringList& declarations) {
  const ProcessResult current = runPfctl({showOption}, {}, true);
  if (current.exitCode != 0) {
    return false;
  }
  QStringList rules = withoutOwnRootDeclarations(current.standardOutput,
                                                  keywords);
  rules.append(declarations);
  return runPfctl({loadOption, QStringLiteral("-f"), QStringLiteral("-")},
                  rules.join('\n').toUtf8() + '\n')
             .exitCode == 0;
}

bool removeRootAnchorDeclarations() {
  const bool nat = replaceRootRules(QStringLiteral("-sn"),
                                    QStringLiteral("-N"),
                                    {QStringLiteral("nat-anchor"),
                                     QStringLiteral("rdr-anchor")}, {});
  const bool filter = replaceRootRules(QStringLiteral("-sr"),
                                       QStringLiteral("-R"),
                                       {QStringLiteral("anchor")}, {});
  return nat && filter;
}

QString tokenPath() {
  return QDir(DaemonDataDir).filePath(QStringLiteral("pf.token"));
}

QString readToken() {
  QFile file(tokenPath());
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  const QString token = QString::fromLatin1(file.readAll()).trimmed();
  static const QRegularExpression validToken(QStringLiteral("^[0-9a-fA-F]+$"));
  return validToken.match(token).hasMatch() ? token : QString();
}

bool releaseToken() {
  if (!QFileInfo::exists(tokenPath())) {
    return true;
  }
  const QString token = readToken();
  if (token.isEmpty()) {
    logger.warning() << "PF token file is invalid";
    return false;
  }
  if (runPfctl({QStringLiteral("-X"), token}, {}, true).exitCode != 0) {
    logger.warning() << "Failed to release PF token";
    return false;
  }
  return QFile::remove(tokenPath());
}
}  // namespace

int MacOSFirewall::execute(const QString& command, bool ignoreErrors) {
  return runProcess(QStringLiteral("/bin/bash"),
                    {QStringLiteral("-c"), command}, {}, ignoreErrors)
      .exitCode;
}

void MacOSFirewall::installRootAnchors() {
  logger.info() << "Installing PF root anchors";

  const QString loadDirective =
      QStringLiteral("load anchor \"%1\" from \"%2/%3.conf\"")
          .arg(kRootAnchor, ResourceDir, kResourceAnchor);
  replaceRootRules(QStringLiteral("-sn"), QStringLiteral("-N"),
                   {QStringLiteral("nat-anchor"),
                    QStringLiteral("rdr-anchor")},
                   {QStringLiteral("nat-anchor \"%1/*\"").arg(kRootAnchor),
                    QStringLiteral("rdr-anchor \"%1/*\"").arg(kRootAnchor),
                    loadDirective});
  replaceRootRules(QStringLiteral("-sr"), QStringLiteral("-R"),
                   {QStringLiteral("anchor")},
                   {QStringLiteral("anchor \"%1/*\"").arg(kRootAnchor),
                    loadDirective});
}

void MacOSFirewall::install() {
  if (!kIsSideBySide) {
    execute(QStringLiteral(
                "if grep -Fq '%1' /etc/pf.conf; then "
                "grep -vF '%1' /etc/pf.conf > /etc/pf.conf.amnezia.tmp && "
                "mv /etc/pf.conf.amnezia.tmp /etc/pf.conf; fi")
                .arg(kRootAnchor));
  }

  uninstall();
  if (QFileInfo::exists(tokenPath())) {
    logger.warning() << "PF token cleanup failed; installation aborted";
    return;
  }
  timespec waitTime{0, 10'000'000};
  ::nanosleep(&waitTime, nullptr);

  installRootAnchors();
  if (!QDir().mkpath(DaemonDataDir)) {
    logger.warning() << "Failed to create PF data directory";
    return;
  }
  const ProcessResult enabled =
      runProcess(QStringLiteral("/sbin/pfctl"), {QStringLiteral("-E")}, {},
                 false, false);
  const QByteArray combined = enabled.standardOutput + '\n' +
                              enabled.standardError;
  const QRegularExpression tokenPattern(
      QStringLiteral("Token\\s*:\\s*([0-9a-fA-F]+)"));
  const QRegularExpressionMatch match =
      tokenPattern.match(QString::fromLatin1(combined));
  if (enabled.exitCode == 0 && match.hasMatch()) {
    const QString token = match.captured(1);
    const QByteArray tokenBytes = token.toLatin1();
    QFile file(tokenPath());
    const bool stored = file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                        file.write(tokenBytes) == tokenBytes.size() &&
                        file.flush();
    if (file.isOpen()) {
      file.close();
    }
    if (!stored) {
      QFile::remove(tokenPath());
      runPfctl({QStringLiteral("-X"), token}, {}, true);
    }
  }
}

void MacOSFirewall::uninstall() {
  logger.info() << "Uninstalling PF root anchor";

  runPfctl({QStringLiteral("-q"), QStringLiteral("-a"), kRootAnchor + "/*",
            QStringLiteral("-F"), QStringLiteral("all")}, {}, true);
  releaseToken();
  if (kIsSideBySide) {
    removeRootAnchorDeclarations();
  } else {
    execute(QStringLiteral(
                "test -f /etc/pf.conf && pfctl -F all -f /etc/pf.conf"),
            true);
  }
}

bool MacOSFirewall::isInstalled() {
  return isPFEnabled() && isRootAnchorLoaded();
}

bool MacOSFirewall::isPFEnabled() {
  const QString token = readToken();
  if (token.isEmpty()) {
    return false;
  }
  const ProcessResult references =
      runPfctl({QStringLiteral("-s"), QStringLiteral("References")}, {}, true);
  return references.exitCode == 0 &&
         QString::fromUtf8(references.standardOutput).contains(token);
}

void MacOSFirewall::ensureRootAnchorPriority() {
  const ProcessResult current = runPfctl({QStringLiteral("-sr")}, {}, true);
  if (current.exitCode != 0) {
    return;
  }
  const QStringList rules = QString::fromUtf8(current.standardOutput)
                                .split('\n', Qt::SkipEmptyParts);
  if (!rules.isEmpty() &&
      isOwnRootDeclaration(rules.constLast(), QStringLiteral("anchor"))) {
    return;
  }
  replaceRootRules(QStringLiteral("-sr"), QStringLiteral("-R"),
                   {QStringLiteral("anchor")},
                   {QStringLiteral("anchor \"%1/*\"").arg(kRootAnchor)});
}

bool MacOSFirewall::isRootAnchorLoaded() {
  const ProcessResult rootRules = runPfctl({QStringLiteral("-sr")}, {}, true);
  bool declared = false;
  for (const QString& line : QString::fromUtf8(rootRules.standardOutput)
                                 .split('\n', Qt::SkipEmptyParts)) {
    if (isOwnRootDeclaration(line, QStringLiteral("anchor"))) {
      declared = true;
      break;
    }
  }
  const ProcessResult anchorRules =
      runPfctl({QStringLiteral("-q"), QStringLiteral("-a"), kRootAnchor,
                QStringLiteral("-s"), QStringLiteral("rules")}, {}, true);
  return rootRules.exitCode == 0 && declared && anchorRules.exitCode == 0 &&
         !anchorRules.standardOutput.isEmpty();
}

void MacOSFirewall::enableAnchor(const QString& anchor) {
  if (isAnchorEnabled(anchor)) {
    return;
  }
  runPfctl({QStringLiteral("-q"), QStringLiteral("-a"),
            kRootAnchor + '/' + anchor, QStringLiteral("-F"),
            QStringLiteral("all"), QStringLiteral("-f"),
            ResourceDir + '/' + kResourceAnchor + '.' + anchor + ".conf"});
}

void MacOSFirewall::disableAnchor(const QString& anchor) {
  runPfctl({QStringLiteral("-q"), QStringLiteral("-a"),
            kRootAnchor + '/' + anchor, QStringLiteral("-F"),
            QStringLiteral("all")}, {}, true);
}

bool MacOSFirewall::isAnchorEnabled(const QString& anchor) {
  const ProcessResult rules =
      runPfctl({QStringLiteral("-q"), QStringLiteral("-a"),
                kRootAnchor + '/' + anchor, QStringLiteral("-s"),
                QStringLiteral("rules")}, {}, true);
  return rules.exitCode == 0 && !rules.standardOutput.isEmpty();
}

void MacOSFirewall::setAnchorEnabled(const QString& anchor, bool enabled) {
  if (enabled) {
    enableAnchor(anchor);
  } else {
    disableAnchor(anchor);
  }
}

void MacOSFirewall::setAnchorTable(const QString& anchor, bool enabled,
                                   const QString& table,
                                   const QStringList& items) {
  QStringList arguments{QStringLiteral("-q"), QStringLiteral("-a"),
                        kRootAnchor + '/' + anchor, QStringLiteral("-t"),
                        table, QStringLiteral("-T")};
  arguments.append(enabled ? QStringLiteral("replace")
                           : QStringLiteral("kill"));
  if (enabled) {
    arguments.append(items);
  }
  runPfctl(arguments, {}, !enabled);
}

void MacOSFirewall::setAnchorWithRules(const QString& anchor, bool enabled,
                                       const QStringList& ruleList) {
  const QString runtimeAnchor = kRootAnchor + '/' + anchor;
  if (!enabled) {
    runPfctl({QStringLiteral("-q"), QStringLiteral("-a"), runtimeAnchor,
              QStringLiteral("-F"), QStringLiteral("rules")}, {}, true);
    return;
  }
  runPfctl({QStringLiteral("-q"), QStringLiteral("-a"), runtimeAnchor,
            QStringLiteral("-f"), QStringLiteral("-")},
           ruleList.join('\n').toUtf8() + '\n', true);
}

void MacOSFirewall::setRouterBypassEnabled(bool enabled) {
  setAnchorWithRules(QStringLiteral("150.allowExcludedApps"), enabled,
                     {QStringLiteral(
                         "pass out quick proto { tcp, udp } user root flags "
                         "any no state")});
}
