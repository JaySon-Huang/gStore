/*
 * Triple.cpp
 *
 *  Created on: 2014-6-20
 *      Author: liyouhuan
 */

#include "Triple.h"

Triple::Triple(const string _s, const string _p, const string _o)
{
	this->subject = _s;
	this->predicate = _p;
	this->object = _o;
}

/* _line responds to a triple
 * It's OK if _line ends up with '.' or not
 * tuples separated by '\t'
 *  */
Triple::Triple(string _line)
{
	size_t _first_tab = _line.find('\t');
	size_t _second_tab = _line.find('\t', _first_tab+1);

	int _sub_size = _first_tab - 0;
	int _pre_size = _second_tab - (_first_tab+1);
	this->subject = _line.substr(0, _sub_size);
	this->predicate = _line.substr(_first_tab+1, _pre_size);

	int _obj_size = -1;
	char _last_c = _line.at(_line.length()-1);

	/* make sure that '.' is not included,
	 * if exist at last character */
	if(_last_c != '.'){
		_obj_size = _line.length() - (_second_tab+1);
	}else{
		_obj_size = (_line.length()-1) - (_second_tab+1);
	}

	this->object = _line.substr(_second_tab+1, _obj_size);
}

Triple::Triple()
{
	this->subject = "";
	this->predicate = "";
	this->object = "";
}
Triple::Triple(const Triple& _triple)
{
	this->subject = _triple.subject;
	this->predicate = _triple.predicate;
	this->object  = _triple.object;
}
Triple& Triple::operator=(const Triple& _triple)
{
	this->subject = _triple.subject;
	this->predicate = _triple.predicate;
	this->object  = _triple.object;
	return *this;
}

void Triple::setSubject(const string& _s){
	this->subject= _s;
}

void Triple::setPredicate(const string& _p){
	this->predicate= _p;
}

void Triple::setObject(const string& _o){
	this->object= _o;
}

const string& Triple::getSubject()const{
	return this->subject;
}

const string& Triple::getPredicate()const{
	return this->predicate;
}

const string& Triple::getObject()const{
	return this->object;
}

const string Triple::toString()const{
	return this->subject+"\t"+
			this->predicate+"\t"+
			this->object+".";
}
