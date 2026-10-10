#pragma once
#include <Arduino.h>
#include <math.h>

struct SolarMetrics {
    float elevationDegrees;
    bool isDaylight;
    float circadianCctRatio; //0.0 deep ember, 1.0 full daylight
};

struct RespiratoryComfort {
    float vpdKpa;
    const char* comfortLabel;
    uint16_t statusColor; //RGB565 color for comfort level
};

class EnvironmentalMath {
    public:
        // Compute solar metrics 
        static SolarMetrics computeSolar(float latitude, float longitude, int doy, float utcHours) {
            float latRad = latitude * (M_PI / 180.0f);
            float gamma = (2.0f * M_PI / 365.0f) * (doy - 1 + (utcHours - 12.0f) / 24.0f);

            //equation of time
            float eqTime = 229.18f * (0.000075f + 0.001868f * cosf(gamma) - 0.032077f * sinf(gamma) - 0.014615f * cosf(2.0f * gamma) - 0.040849f * sinf(2.0f * gamma));

            float decl = 0.006918f - 0.399912f * cosf(gamma) + 0.070257f * sinf(gamma) - 0.006758f * cosf(2.0f * gamma) + 0.000907f * sinf(2.0f * gamma) - 0.002697f * cosf(3.0f * gamma) + 0.00148f * sinf(3.0f * gamma);

            float timeOffset = eqTime + (4.0f * longitude);
            float tst = fmodf((utcHours * 60.0f + timeOffset), 1440.0f);
            if (tst < 0.0f) tst += 1440.0f;
            float hourAngleRad = (tst / 4.0f - 180.0f) * (M_PI / 180.0f);

            //solar elevation angle
            float sinElevation = sinf(latRad) * sinf(decl) + cosf(latRad) * cosf(decl) * cosf(hourAngleRad);
            float elevationDegrees = asinf(sinElevation) * (180.0f / M_PI);

            SolarMetrics metrics;
            metrics.elevationDegrees = elevationDegrees;
            metrics.isDaylight = elevationDegrees > 0.0f;

            //circadian CCT ratio
            if (elevationDegrees < -6.0f) {
                metrics.circadianCctRatio = 0.0f;
            } else if (elevationDegrees > 35.0f) {
                metrics.circadianCctRatio = 1.0f;
            } else {
                metrics.circadianCctRatio = (elevationDegrees + 6.0f) / 41.0f;
            }
            return metrics;
        }

        // Compute vapor pressure deficit (VPD) and comfort level
        static RespiratoryComfort computeVPD(float temperatureC, float relativeHumidity) {
            float vpSaturation = 0.61078f * expf((17.27f * temperatureC) / (temperatureC + 237.3f));

            float vpActual = vpSaturation * (relativeHumidity / 100.0f);
            float vpd = vpSaturation - vpActual;

            RespiratoryComfort result;
            result.vpdKpa = vpd;

            if (vpd < 0.35f) {
                result.comfortLabel = "MUGGY / CONDENSE";
                result.statusColor = 0x03FF;
            } else if (vpd < 1.10f) {
                result.comfortLabel = "OPTIMAL";
                result.statusColor = 0x07E0;
            } else if (vpd < 1.60f) {
                result.comfortLabel = "DRY";
                result.statusColor = 0xFDA0;
            } else {
                result.comfortLabel = "VERY DRY";
                result.statusColor = 0xF800;
            }
            return result;
        }
};