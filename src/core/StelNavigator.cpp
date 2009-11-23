/*
 * Stellarium
 * Copyright (C) 2003 Fabien Chereau
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include "StelApp.hpp"
#include "StelNavigator.hpp"
#include "StelUtils.hpp"
#include "SolarSystem.hpp"
#include "StelObserver.hpp"
#include "Planet.hpp"
#include "StelObjectMgr.hpp"
#include "StelCore.hpp"
#include "StelLocation.hpp"
#include "StelLocationMgr.hpp"
#include "StelModuleMgr.hpp"
#include "StelMovementMgr.hpp"

#include <QSettings>
#include <QStringList>
#include <QDateTime>
#include <QDebug>

// Init statics transfo matrices
// See vsop87.doc:
const Mat4d StelNavigator::matJ2000ToVsop87(Mat4d::xrotation(-23.4392803055555555556*(M_PI/180)) * Mat4d::zrotation(0.0000275*(M_PI/180)));
const Mat4d StelNavigator::matVsop87ToJ2000(matJ2000ToVsop87.transpose());
const Mat4d StelNavigator::matJ2000toGalactic(-0.054875539726, 0.494109453312, -0.867666135858, 0, -0.873437108010, -0.444829589425, -0.198076386122, 0, -0.483834985808, 0.746982251810, 0.455983795705, 0, 0, 0, 0, 1);

////////////////////////////////////////////////////////////////////////////////
StelNavigator::StelNavigator() : timeSpeed(JD_SECOND), JDay(0.), position(NULL)
{
	altAzVisionDirection=Vec3d(1.,0.,0.);
	earthEquVisionDirection=Vec3d(1.,0.,0.);
	J2000EquVisionDirection=Vec3d(1.,0.,0.);  // not correct yet...
	mountMode = MountAltAzimuthal;  // default
}

StelNavigator::~StelNavigator()
{
	delete position;
	position=NULL;
}

void StelNavigator::init()
{
	QSettings* conf = StelApp::getInstance().getSettings();
	Q_ASSERT(conf);

	defaultLocationID = conf->value("init_location/location","Paris, Paris, France").toString();
	position = new StelObserver(StelApp::getInstance().getLocationMgr().locationForSmallString(defaultLocationID));

	setTimeNow();
	setAltAzVisionDirection(Vec3f(1,1e-05,0.2));
	// Compute transform matrices between coordinates systems
	updateTransformMatrices();
	updateModelViewMat();
	QString tmpstr = conf->value("navigation/viewing_mode", "horizon").toString();
	if (tmpstr=="equator")
		setMountMode(StelNavigator::MountEquatorial);
	else
	{
		if (tmpstr=="horizon")
			setMountMode(StelNavigator::MountAltAzimuthal);
		else
		{
			qDebug() << "ERROR : Unknown viewing mode type : " << tmpstr;
			Q_ASSERT(0);
		}
	}

	initViewPos = StelUtils::strToVec3f(conf->value("navigation/init_view_pos").toString());
	setAltAzVisionDirection(initViewPos);

	// we want to be able to handle the old style preset time, recorded as a double
	// jday, or as a more human readable string...
	bool ok;
	QString presetTimeStr = conf->value("navigation/preset_sky_time",2451545.).toString();
	presetSkyTime = presetTimeStr.toDouble(&ok);
	if (ok)
		qDebug() << "navigation/preset_sky_time is a double - treating as jday:" << presetSkyTime;
	else
	{
		qDebug() << "navigation/preset_sky_time was not a double, treating as string date:" << presetTimeStr;
		presetSkyTime = StelUtils::qDateTimeToJd(QDateTime::fromString(presetTimeStr));
	}

	// Navigation section
	setInitTodayTime(QTime::fromString(conf->value("navigation/today_time", "22:00").toString()));

	startupTimeMode = conf->value("navigation/startup_time_mode", "actual").toString().toLower();
	if (startupTimeMode=="preset")
		setJDay(presetSkyTime - StelUtils::getGMTShiftFromQT(presetSkyTime) * JD_HOUR);
	else if (startupTimeMode=="today")
		setTodayTime(getInitTodayTime());

	// we previously set the time to "now" already, so we don't need to
	// explicitly do it if the startupTimeMode=="now".
}

// Set the location to use by default at startup
void StelNavigator::setDefaultLocationID(const QString& id)
{
	defaultLocationID = id;
	StelApp::getInstance().getLocationMgr().locationForSmallString(id);
	QSettings* conf = StelApp::getInstance().getSettings();
	Q_ASSERT(conf);
	conf->setValue("init_location/location", id);
}

//! Set stellarium time to current real world time
void StelNavigator::setTimeNow()
{
	setJDay(StelUtils::getJDFromSystem());
}

void StelNavigator::setTodayTime(const QTime& target)
{
	QDateTime dt = QDateTime::currentDateTime();
	if (target.isValid())
	{
		dt.setTime(target);
		// don't forget to adjust for timezone / daylight savings.
		setJDay(StelUtils::qDateTimeToJd(dt)-(StelUtils::getGMTShiftFromQT(StelUtils::getJDFromSystem()) * JD_HOUR));
	}
	else
	{
		qWarning() << "WARNING - time passed to StelNavigator::setTodayTime is not valid. The system time will be used." << target;
		setTimeNow();
	}
}

//! Get whether the current stellarium time is the real world time
bool StelNavigator::getIsTimeNow(void) const
{
	// cache last time to prevent to much slow system call
	static double lastJD = getJDay();
	static bool previousResult = (fabs(getJDay()-StelUtils::getJDFromSystem())<JD_SECOND);
	if (fabs(lastJD-getJDay())>JD_SECOND/4)
	{
		lastJD = getJDay();
		previousResult = (fabs(getJDay()-StelUtils::getJDFromSystem())<JD_SECOND);
	}
	return previousResult;
}

void StelNavigator::addSolarDays(double d)
{
	setJDay(getJDay() + d);
}

void StelNavigator::addSiderealDays(double d)
{
	const PlanetP& home = position->getHomePlanet();
	if (home->getEnglishName() != "Solar System StelObserver")
		d *= home->getSiderealDay();
	setJDay(getJDay() + d);
}

void StelNavigator::moveObserverToSelected(void)
{
	StelObjectMgr* objmgr = GETSTELMODULE(StelObjectMgr);
	Q_ASSERT(objmgr);
	if (objmgr->getWasSelected())
	{
		Planet* pl = dynamic_cast<Planet*>(objmgr->getSelectedObject()[0].data());
		if (pl)
		{
			// We need to move to the selected planet. Try to generate a location from the current one
			StelLocation loc = getCurrentLocation();
			loc.planetName = pl->getEnglishName();
			loc.name = "-";
			loc.state = "";
			moveObserverTo(loc);
		}
	}
	StelMovementMgr* mmgr = GETSTELMODULE(StelMovementMgr);
	Q_ASSERT(mmgr);
	mmgr->setFlagTracking(false);
}

// Get the informations on the current location
const StelLocation& StelNavigator::getCurrentLocation() const
{
	return position->getCurrentLocation();
}

// Smoothly move the observer to the given location
void StelNavigator::moveObserverTo(const StelLocation& target, double duration, double durationIfPlanetChange)
{
	double d = (getCurrentLocation().planetName==target.planetName) ? duration : durationIfPlanetChange;
	if (d>0.)
	{
		SpaceShipObserver* newObs = new SpaceShipObserver(getCurrentLocation(), target, d);
		delete position;
		position = newObs;
		newObs->update(0);
	}
	else
	{
		delete position;
		position = new StelObserver(target);
	}
	emit(locationChanged(target));
}

// Get the sideral time shifted by the observer longitude
double StelNavigator::getLocalSideralTime() const
{
	return (position->getHomePlanet()->getSiderealTime(JDay)+position->getCurrentLocation().longitude)*M_PI/180.;
}

//! Get the duration of a sideral day for the current observer in day.
double StelNavigator::getLocalSideralDayLength() const
{
	return position->getHomePlanet()->getSiderealDay();
}

void StelNavigator::setInitViewDirectionToCurrent(void)
{
	initViewPos = altAzVisionDirection;
	QString dirStr = QString("%1,%2,%3").arg(altAzVisionDirection[0]).arg(altAzVisionDirection[1]).arg(altAzVisionDirection[2]);
	StelApp::getInstance().getSettings()->setValue("navigation/init_view_pos", dirStr);
}

//! Increase the time speed
void StelNavigator::increaseTimeSpeed()
{
	double s = getTimeRate();
	if (s>=JD_SECOND) s*=10.;
	else if (s<-JD_SECOND) s/=10.;
	else if (s>=0. && s<JD_SECOND) s=JD_SECOND;
	else if (s>=-JD_SECOND && s<0.) s=0.;
	setTimeRate(s);
}

//! Decrease the time speed
void StelNavigator::decreaseTimeSpeed()
{
	double s = getTimeRate();
	if (s>JD_SECOND) s/=10.;
	else if (s<=-JD_SECOND) s*=10.;
	else if (s>-JD_SECOND && s<=0.) s=-JD_SECOND;
	else if (s>0. && s<=JD_SECOND) s=0.;
	setTimeRate(s);
}

void StelNavigator::increaseTimeSpeedLess()
{
	double s = getTimeRate();
	if (s>=JD_SECOND) s*=2.;
	else if (s<-JD_SECOND) s/=2.;
	else if (s>=0. && s<JD_SECOND) s=JD_SECOND;
	else if (s>=-JD_SECOND && s<0.) s=0.;
	setTimeRate(s);
}

void StelNavigator::decreaseTimeSpeedLess()
{
	double s = getTimeRate();
	if (s>JD_SECOND) s/=2.;
	else if (s<=-JD_SECOND) s*=2.;
	else if (s>-JD_SECOND && s<=0.) s=-JD_SECOND;
	else if (s>0. && s<=JD_SECOND) s=0.;
	setTimeRate(s);
}

////////////////////////////////////////////////////////////////////////////////
void StelNavigator::setAltAzVisionDirection(const Vec3d& _pos)
{
	altAzVisionDirection = _pos;
	earthEquVisionDirection=altAzToEquinoxEqu(altAzVisionDirection);
	J2000EquVisionDirection = matEquinoxEquToJ2000*earthEquVisionDirection;
	updateModelViewMat();
}

////////////////////////////////////////////////////////////////////////////////
void StelNavigator::setEquinoxEquVisionDirection(const Vec3d& _pos)
{
	earthEquVisionDirection = _pos;
	J2000EquVisionDirection = matEquinoxEquToJ2000*earthEquVisionDirection;
	altAzVisionDirection = equinoxEquToAltAz(earthEquVisionDirection);
	updateModelViewMat();
}

////////////////////////////////////////////////////////////////////////////////
void StelNavigator::setJ2000EquVisionDirection(const Vec3d& _pos)
{
	J2000EquVisionDirection = _pos;
	earthEquVisionDirection = matJ2000ToEquinoxEqu*J2000EquVisionDirection;
	altAzVisionDirection = equinoxEquToAltAz(earthEquVisionDirection);
	updateModelViewMat();
}

////////////////////////////////////////////////////////////////////////////////
// Increment time
void StelNavigator::updateTime(double deltaTime)
{
	JDay+=timeSpeed*deltaTime;

	// Fix time limits to -100000 to +100000 to prevent bugs
	if (JDay>38245309.499988) JDay = 38245309.499988;
	if (JDay<-34803211.500012) JDay = -34803211.500012;

	if (position->isObserverLifeOver())
	{
		// Unselect if the new home planet is the previously selected object
		StelObjectMgr* objmgr = GETSTELMODULE(StelObjectMgr);
		Q_ASSERT(objmgr);
		if (objmgr->getWasSelected() && objmgr->getSelectedObject()[0].data()==position->getHomePlanet())
		{
			objmgr->unSelect();
		}
		StelObserver* newObs = position->getNextObserver();
		delete position;
		position = newObs;
	}
	position->update(deltaTime);

	// Position of sun and all the satellites (ie planets)
	SolarSystem* solsystem = (SolarSystem*)StelApp::getInstance().getModuleMgr().getModule("SolarSystem");
	solsystem->computePositions(getJDay(), position->getHomePlanet()->getHeliocentricEclipticPos());
}

////////////////////////////////////////////////////////////////////////////////
// The non optimized (more clear version is available on the CVS : before date 25/07/2003)
void StelNavigator::updateTransformMatrices(void)
{
	matAltAzToEquinoxEqu = position->getRotAltAzToEquatorial(JDay);
	matEquinoxEquToAltAz = matAltAzToEquinoxEqu.transpose();

	matEquinoxEquToJ2000 = matVsop87ToJ2000 * position->getRotEquatorialToVsop87();
	matJ2000ToEquinoxEqu = matEquinoxEquToJ2000.transpose();
	matJ2000ToAltAz = matEquinoxEquToAltAz*matJ2000ToEquinoxEqu;

	matHeliocentricEclipticToEquinoxEqu = matJ2000ToEquinoxEqu * matVsop87ToJ2000 * Mat4d::translation(-position->getCenterVsop87Pos());

	// These two next have to take into account the position of the observer on the earth
	Mat4d tmp = matJ2000ToVsop87 * matEquinoxEquToJ2000 * matAltAzToEquinoxEqu;

	matAltAzToHeliocentricEcliptic =  Mat4d::translation(position->getCenterVsop87Pos()) * tmp *
						  Mat4d::translation(Vec3d(0.,0., position->getDistanceFromCenter()));

	matHeliocentricEclipticToAltAz =  Mat4d::translation(Vec3d(0.,0.,-position->getDistanceFromCenter())) * tmp.transpose() *
						  Mat4d::translation(-position->getCenterVsop87Pos());
}

void StelNavigator::setStartupTimeMode(const QString& s)
{
	startupTimeMode = s;
}

////////////////////////////////////////////////////////////////////////////////
// Update the modelview matrices
void StelNavigator::updateModelViewMat(void)
{
	Vec3d f;

	if (mountMode == MountEquatorial)
	{
		// view will use equatorial coordinates, so that north is always up
		f = earthEquVisionDirection;
	}
	else
	{
		// view will correct for horizon (always down)
		f = altAzVisionDirection;
	}

	f.normalize();
	Vec3d s(f[1],-f[0],0.);

	if (mountMode == MountEquatorial)
	{
		// convert everything back to local coord
		f = altAzVisionDirection;
		f.normalize();
		s = equinoxEquToAltAz( s );
	}

	Vec3d u(s^f);
	s.normalize();
	u.normalize();

	matAltAzModelView.set(s[0],u[0],-f[0],0.,
						 s[1],u[1],-f[1],0.,
						 s[2],u[2],-f[2],0.,
						 0.,0.,0.,1.);
}


////////////////////////////////////////////////////////////////////////////////
// Return the observer heliocentric position
Vec3d StelNavigator::getObserverHeliocentricEclipticPos(void) const
{
	return Vec3d(matAltAzToHeliocentricEcliptic[12], matAltAzToHeliocentricEcliptic[13], matAltAzToHeliocentricEcliptic[14]);
}

void StelNavigator::setPresetSkyTime(QDateTime dt)
{
	setPresetSkyTime(StelUtils::qDateTimeToJd(dt));
}
